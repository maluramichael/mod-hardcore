/*
 * mod-hardcore
 *
 * Opt-in permadeath ruleset. A character flags itself Hardcore with a chat
 * command ('.hardcore on'). While flagged, a death is intercepted
 * (OnPlayerJustDied) and the configured death policy (Hardcore.DeathPolicy)
 * is applied:
 *
 *   1. ghost   (DEFAULT, non-destructive) - the character becomes a
 *              permanently locked ghost: OnPlayerCanResurrect and
 *              OnPlayerCanRepopAtGraveyard both refuse for a fallen
 *              character, so it can never be revived or walk to a graveyard
 *              again. Nothing is renamed or deleted.
 *   2. archive (non-destructive) - the character is kicked, then renamed
 *              ("F<guid>") once it has safely logged out; any further login
 *              attempt is immediately kicked again with the fallen message.
 *              Data is kept, nothing is deleted.
 *   3. delete  (DESTRUCTIVE, off by default) - the character is kicked, and
 *              once it has safely logged out it is permanently removed via
 *              Player::DeleteFromDB.
 *
 *              Both Archive's rename and Delete's removal are deliberately
 *              deferred to OnPlayerLogout (which fires AFTER
 *              WorldSession::LogoutPlayer's own SaveToDB), not done inside
 *              OnPlayerJustDied itself: that hook runs deep inside live
 *              death/combat processing on `this` Player, and a rename or
 *              delete performed there would just be overwritten a moment
 *              later by the kick-triggered logout's own SaveToDB (Archive),
 *              or would race the engine's save/logout flow entirely
 *              (Delete).
 *
 * The Hardcore flag and the "fallen" state are both stored in
 * hardcore_flags, keyed by low guid, and cached in memory (loaded once at
 * startup, updated on every '.hardcore on' / death) so the per-hit hooks
 * above never need a DB round trip. Every death is additionally logged to
 * hardcore_deaths for a permanent record.
 *
 * Hardcore is conceptually self-found, but this module does not itself
 * touch trade/mail/AH - see mod-self-found for that (install both for the
 * full WoW-Forever Hardcore ruleset).
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSessionMgr.h"

// Playerbots fork header, used only to recognise a bot-controlled session so we
// don't chat-spam a bot with login/death messages. Enforcement itself is guid
// based and applies the same whether the character is human- or bot-controlled.
#include "Playerbots.h"

#include "Hardcore.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace Hardcore
{
    Config& GetConfig()
    {
        static Config cfg;
        return cfg;
    }
}

using namespace Acore::ChatCommands;

namespace
{
    // In-memory caches. Only ever touched from the world update thread (chat
    // commands and script hooks both run there on this core), so no locking
    // is needed - same assumption mod-self-found's cache relies on.
    std::unordered_set<uint32>& FlaggedGuids()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    std::unordered_set<uint32>& FallenGuids()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    // Guids waiting for a deferred archive-rename once they safely log out
    // (DeathPolicy::Archive only). Deferred past OnPlayerJustDied because a
    // rename done there would be overwritten by the kick-triggered logout's
    // own SaveToDB, which runs before OnPlayerLogout. In-memory only: a
    // crash between the kick and the logout just leaves the character fallen
    // and un-renamed (still non-destructive - it retries next successful
    // logout, since the fallen flag alone already blocks resurrection/login).
    std::unordered_set<uint32>& PendingArchiveGuids()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    // Guids waiting for a deferred Player::DeleteFromDB once they safely log
    // out (DeathPolicy::Delete only). In-memory only: a crash between the
    // kick and the logout just leaves the character fallen-in-place (not
    // deleted), never half-deleted.
    std::unordered_set<uint32>& PendingDeleteGuids()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    void AnnounceDeath(Player* player)
    {
        if (!Hardcore::GetConfig().AnnounceDeaths || !player)
            return;

        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING,
            Acore::StringFormat("|cffff0000[Hardcore]|r {} has fallen and will never rise again.",
                player->GetName()));
    }

    void SendFallenMessage(Player* player)
    {
        if (!player || !player->GetSession())
            return;

        // Bots don't read chat - the message is still useful for the humans
        // controlling their own hardcore altbots, so only skip it when the
        // session is a full random/rented bot, matching mod-self-found.
        if (GET_PLAYERBOT_AI(player))
            return;

        ChatHandler(player->GetSession())
            .SendSysMessage("|cffff0000[Hardcore]|r This hardcore character has fallen and can no longer be played.");
    }
}

namespace Hardcore
{
    void EnsureSchema()
    {
        // Deliberately NOT an SQL update file: on this fork a failing module
        // SQL aborts the whole worldserver boot, so we create the tables
        // programmatically and tolerate failure at runtime instead (matches
        // mod-self-found's / mod-guild-tax's approach).
        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `hardcore_flags` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1, "
            "`fallen` TINYINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `hardcore_deaths` ("
            "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "`guid` INT UNSIGNED NOT NULL, "
            "`name` VARCHAR(12) NOT NULL, "
            "`level` TINYINT UNSIGNED NOT NULL, "
            "`map` INT UNSIGNED NOT NULL, "
            "`zone` INT UNSIGNED NOT NULL, "
            "`ts` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`id`), "
            "KEY `idx_hardcore_deaths_guid` (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    }

    void LoadFlags()
    {
        std::unordered_set<uint32>& flagged = FlaggedGuids();
        std::unordered_set<uint32>& fallen = FallenGuids();
        flagged.clear();
        fallen.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `enabled`, `fallen` FROM `hardcore_flags`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 guidLow = fields[0].Get<uint32>();

                if (fields[1].Get<uint8>() != 0)
                    flagged.insert(guidLow);
                if (fields[2].Get<uint8>() != 0)
                    fallen.insert(guidLow);
            } while (result->NextRow());
        }
    }

    bool IsHardcoreGuid(uint32_t guidLow)
    {
        std::unordered_set<uint32> const& guids = FlaggedGuids();
        return guids.find(guidLow) != guids.end();
    }

    bool IsHardcore(Player* player)
    {
        if (!player)
            return false;

        return IsHardcoreGuid(player->GetGUID().GetCounter());
    }

    bool IsFallenGuid(uint32_t guidLow)
    {
        std::unordered_set<uint32> const& guids = FallenGuids();
        return guids.find(guidLow) != guids.end();
    }

    bool IsFallen(Player* player)
    {
        if (!player)
            return false;

        return IsFallenGuid(player->GetGUID().GetCounter());
    }

    void SetHardcore(Player* player, bool enabled)
    {
        if (!player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();

        CharacterDatabase.Execute(
            "INSERT INTO `hardcore_flags` (`guid`, `enabled`) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE `enabled` = {}",
            guidLow, uint32(enabled ? 1 : 0), uint32(enabled ? 1 : 0));

        if (enabled)
            FlaggedGuids().insert(guidLow);
        else
            FlaggedGuids().erase(guidLow);
    }

    void HandleDeath(Player* player)
    {
        if (!player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();

        // Not Hardcore, or already processed this death (e.g. a stray second
        // OnPlayerJustDied while already a locked ghost) - nothing to do.
        if (!IsHardcoreGuid(guidLow) || IsFallenGuid(guidLow))
            return;

        Config const& cfg = GetConfig();

        // Log the death permanently, regardless of policy.
        CharacterDatabase.Execute(
            "INSERT INTO `hardcore_deaths` (`guid`, `name`, `level`, `map`, `zone`, `ts`) "
            "VALUES ({}, '{}', {}, {}, {}, {})",
            guidLow, player->GetName(), uint32(player->GetLevel()), player->GetMapId(), player->GetZoneId(),
            uint32(GameTime::GetGameTime().count()));

        // Mark fallen (persisted + cached) before touching anything else, so
        // the resurrect/repop-blocking hooks below take effect immediately.
        CharacterDatabase.Execute(
            "INSERT INTO `hardcore_flags` (`guid`, `enabled`, `fallen`) VALUES ({}, 1, 1) "
            "ON DUPLICATE KEY UPDATE `fallen` = 1",
            guidLow);
        FallenGuids().insert(guidLow);

        AnnounceDeath(player);
        SendFallenMessage(player);

        switch (cfg.Policy)
        {
            case DeathPolicy::Ghost:
                // Nothing further: OnPlayerCanResurrect / OnPlayerCanRepopAtGraveyard
                // now permanently refuse for this guid, so the character stays
                // a ghost where it died.
                break;

            case DeathPolicy::Archive:
                // Non-destructive, deferred: queue for rename, kick now. The
                // actual rename happens in HandleLogout, once
                // WorldSession::LogoutPlayer has finished its own save (which
                // would otherwise overwrite a rename done here with the
                // still-in-memory old name) and the character is safely out
                // of the world.
                PendingArchiveGuids().insert(guidLow);

                if (WorldSession* session = player->GetSession())
                    session->KickPlayer("Hardcore character has fallen (archived)");
                break;

            case DeathPolicy::Delete:
                // Destructive, deferred: queue for delete, kick now. The
                // actual Player::DeleteFromDB happens in HandleLogout, once
                // WorldSession::LogoutPlayer has finished its own save and
                // the character is safely out of the world.
                PendingDeleteGuids().insert(guidLow);

                if (WorldSession* session = player->GetSession())
                    session->KickPlayer("Hardcore character has fallen (to be deleted)");
                break;
        }
    }

    void HandleLogout(Player* player)
    {
        if (!player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();

        // WorldSession::LogoutPlayer has already called _player->SaveToDB()
        // by the time OnPlayerLogout fires, and the character is no longer
        // attached to any map/session logic beyond this teardown - safe to
        // touch its DB rows directly now.

        std::unordered_set<uint32>& pendingArchive = PendingArchiveGuids();
        if (auto it = pendingArchive.find(guidLow); it != pendingArchive.end())
        {
            pendingArchive.erase(it);

            // "F" + guid low part fits the 12-char name column for any guid
            // and is guaranteed unique per guid.
            std::string archivedName = "F" + std::to_string(guidLow);
            if (archivedName.size() > 12)
                archivedName.resize(12);

            CharacterDatabase.Execute("UPDATE `characters` SET `name` = '{}' WHERE `guid` = {}",
                archivedName, guidLow);
        }

        std::unordered_set<uint32>& pendingDelete = PendingDeleteGuids();
        if (auto it = pendingDelete.find(guidLow); it != pendingDelete.end())
        {
            pendingDelete.erase(it);

            WorldSession* session = player->GetSession();
            uint32 accountId = session ? session->GetAccountId() : 0;

            Player::DeleteFromDB(ObjectGuid::LowType(guidLow), accountId, true, true);

            CharacterDatabase.Execute("DELETE FROM `hardcore_flags` WHERE `guid` = {}", guidLow);
            FlaggedGuids().erase(guidLow);
            FallenGuids().erase(guidLow);
        }
    }
}

// =====================================================================
//  CommandScript: `.hardcore on|status`
// =====================================================================
class hardcore_commandscript : public CommandScript
{
public:
    hardcore_commandscript() : CommandScript("hardcore_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hardcoreTable =
        {
            { "on",     HandleHardcoreOnCommand,     SEC_PLAYER, Console::No },
            { "status", HandleHardcoreStatusCommand, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "hardcore", hardcoreTable },
        };

        return commandTable;
    }

    // v1 scope note: opt-in is a one-way commitment by design (no '.hardcore
    // off') - toggling permadeath off would defeat the point of the ruleset.
    static bool HandleHardcoreOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Hardcore::Config const& cfg = Hardcore::GetConfig();
        if (!cfg.Enable)
        {
            handler->SendSysMessage("Hardcore is currently disabled on this server.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (Hardcore::IsFallen(player))
        {
            handler->SendSysMessage("This character has already fallen and cannot be flagged again.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (Hardcore::IsHardcore(player))
        {
            handler->SendSysMessage("This character is already flagged Hardcore.");
            return true;
        }

        if (player->GetLevel() < cfg.MinLevelToFlag)
        {
            handler->PSendSysMessage("You must be at least level {} to opt into Hardcore.", cfg.MinLevelToFlag);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Hardcore::SetHardcore(player, true);
        handler->PSendSysMessage(
            "Hardcore ENABLED for {}. This character permanently falls on death - there is no undo.",
            player->GetName());
        return true;
    }

    static bool HandleHardcoreStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (Hardcore::IsFallen(player))
            handler->SendSysMessage("Hardcore: FALLEN (this character is permanently dead).");
        else if (Hardcore::IsHardcore(player))
            handler->SendSysMessage("Hardcore: ENABLED (permadeath is active).");
        else
            handler->SendSysMessage("Hardcore: disabled.");
        return true;
    }
};

// =====================================================================
//  PlayerScript: death handling, login reminder, resurrection lockout.
// =====================================================================
class HardcorePlayerScript : public PlayerScript
{
public:
    HardcorePlayerScript() : PlayerScript("Hardcore_PlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!Hardcore::GetConfig().Enable || !player || !Hardcore::IsFallen(player))
            return;

        SendFallenMessage(player);

        // Archive/Delete both mean "cannot be played anymore"; kick straight
        // back out even if a stale/renamed fallen character was somehow
        // logged into. Ghost policy deliberately leaves the character
        // logged in - it is meant to stay a locked, visible ghost.
        if (Hardcore::GetConfig().Policy != Hardcore::DeathPolicy::Ghost)
        {
            if (WorldSession* session = player->GetSession())
                session->KickPlayer("Hardcore character has fallen");
        }
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!Hardcore::GetConfig().Enable)
            return;

        Hardcore::HandleDeath(player);
    }

    [[nodiscard]] bool OnPlayerCanResurrect(Player* player) override
    {
        if (!Hardcore::GetConfig().Enable)
            return true;

        return !Hardcore::IsFallen(player);
    }

    [[nodiscard]] bool OnPlayerCanRepopAtGraveyard(Player* player) override
    {
        if (!Hardcore::GetConfig().Enable)
            return true;

        return !Hardcore::IsFallen(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        Hardcore::HandleLogout(player);
    }
};

// =====================================================================
//  WorldScript: config load + schema/cache bootstrap.
// =====================================================================
class HardcoreWorldScript : public WorldScript
{
public:
    HardcoreWorldScript() : WorldScript("Hardcore_WorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Hardcore::Config& cfg = Hardcore::GetConfig();
        cfg.Enable = sConfigMgr->GetOption<bool>("Hardcore.Enable", true);

        std::string policy = ToLower(sConfigMgr->GetOption<std::string>("Hardcore.DeathPolicy", "ghost"));
        if (policy == "archive")
            cfg.Policy = Hardcore::DeathPolicy::Archive;
        else if (policy == "delete")
            cfg.Policy = Hardcore::DeathPolicy::Delete;
        else
            cfg.Policy = Hardcore::DeathPolicy::Ghost; // default, also the fallback for any unrecognised value

        cfg.MinLevelToFlag = sConfigMgr->GetOption<uint32>("Hardcore.MinLevelToFlag", 1);
        cfg.AnnounceDeaths = sConfigMgr->GetOption<bool>("Hardcore.AnnounceDeaths", true);
    }

    void OnStartup() override
    {
        Hardcore::EnsureSchema();
        Hardcore::LoadFlags();
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddHardcoreScripts()
{
    new hardcore_commandscript();
    new HardcorePlayerScript();
    new HardcoreWorldScript();
}
