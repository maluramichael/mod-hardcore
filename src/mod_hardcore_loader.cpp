/*
 * mod-hardcore loader.
 *
 * The playerbots fork auto-globs every module's sources into one lib and looks
 * up a loader symbol derived from the folder name: for folder "mod-hardcore"
 * that symbol is exactly "Addmod_hardcoreScripts". It must exist and call our
 * real registration function.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

void AddHardcoreScripts();

void Addmod_hardcoreScripts()
{
    AddHardcoreScripts();
}
