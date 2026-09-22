/*
 * mod-hardcore - shared declarations.
 *
 * Opt-in permadeath ruleset. A character flagged Hardcore ('.hardcore on') that
 * dies falls under the configured death policy (Hardcore.DeathPolicy):
 *
 *   - ghost   (default, non-destructive): the character is locked as a ghost
 *             forever - resurrection and graveyard release are blocked.
 *   - archive (non-destructive): the character is renamed and locked out of
 *             play, but its data is kept.
 *   - delete  (DESTRUCTIVE, opt-in only): the character is permanently deleted
 *             after it safely logs out.
 *
 * Hardcore is conceptually self-found (no external gear/gold), but this module
 * only owns the permadeath flag/enforcement - it does not itself block
 * trade/mail/AH (see mod-self-found for that, if installed alongside).
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_HARDCORE_H
#define MOD_HARDCORE_H

#include <cstdint>

class Player;

namespace Hardcore
{
    enum class DeathPolicy : uint8_t
    {
        Ghost,   // permanently stuck as a ghost; resurrection/repop blocked
        Archive, // renamed + locked out of play, data kept
        Delete   // permanently deleted once the character safely logs out
    };

    // Cached config (populated in WorldScript::OnAfterConfigLoad).
    struct Config
    {
        bool Enable             = true;             // module master switch
        DeathPolicy Policy      = DeathPolicy::Ghost; // Hardcore.DeathPolicy
        uint32_t MinLevelToFlag = 1;                 // Hardcore.MinLevelToFlag
        bool AnnounceDeaths     = true;               // Hardcore.AnnounceDeaths
    };

    Config& GetConfig();

    // Creates the programmatic hardcore_flags / hardcore_deaths tables in the
    // characters DB if they do not already exist. Safe to call repeatedly.
    void EnsureSchema();

    // (Re)loads the in-memory flagged/fallen-guid caches from the characters
    // DB. Call once at startup, after EnsureSchema().
    void LoadFlags();

    // True if the given character (by low guid) currently opted into Hardcore.
    // Checks only the in-memory cache - no DB hit.
    bool IsHardcoreGuid(uint32_t guidLow);
    bool IsHardcore(Player* player);

    // True if the given character (by low guid) has already fallen (died while
    // flagged Hardcore) and is locked under the death policy.
    bool IsFallenGuid(uint32_t guidLow);
    bool IsFallen(Player* player);

    // Opts a character into Hardcore: persists it to the characters DB and
    // updates the in-memory cache. No-op for a null player.
    void SetHardcore(Player* player, bool enabled);

    // Records a death, marks the character fallen and applies the configured
    // death policy (rename for Archive, queue-for-delete for Delete; Ghost
    // needs no extra bookkeeping beyond the fallen flag). No-op for a null
    // player or a character not currently flagged Hardcore.
    void HandleDeath(Player* player);

    // Called from OnPlayerLogout: performs the deferred DeleteFromDB for a
    // character that fell under DeathPolicy::Delete, now that it has safely
    // left the world. No-op unless that character is queued for deletion.
    void HandleLogout(Player* player);
}

#endif // MOD_HARDCORE_H
