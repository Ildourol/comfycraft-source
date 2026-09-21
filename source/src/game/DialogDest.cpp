/*
 * DialogDest -- see DialogDest.h for what this is and why it is not dialog_spot.
 */

#include "DialogDest.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"

#include <mutex>

/*
 * THE CURRENT BLOCK. Swapped whole rather than edited, because map threads read
 * it without a lock and every writer is on the world thread. DialogDestData's
 * comment has the long version; it is the same bargain LuaScene makes.
 */
static std::shared_ptr<DialogDestData const> s_data = std::make_shared<DialogDestData>();
static std::mutex s_dataLock;

std::shared_ptr<DialogDestData const> DialogDest::Data()
{
    std::lock_guard<std::mutex> guard(s_dataLock);
    return s_data;
}

void DialogDest::LoadAll()
{
    auto fresh = std::make_shared<DialogDestData>();

    if (std::unique_ptr<QueryResult> res{ WorldDatabase.Query(
            "SELECT `id`, `name` FROM `dialog_dest_list`") })
    {
        do
        {
            Field* f = res->Fetch();
            uint32 const id = f[0].GetUInt32();
            if (!id)
            {
                // AUTO_INCREMENT starts at 1 and `datalong` uses 0 to mean "no
                // list chosen", so a row at 0 is a list no button could ever
                // name. Only hand-written SQL can make one.
                sLog.outErrorDb("Table `dialog_dest_list` has a row with id 0, which no button can reference. Ignoring.");
                continue;
            }
            fresh->lists[id].name = f[1].GetCppString();
        } while (res->NextRow());
    }

    uint32 orphans = 0;

    if (std::unique_ptr<QueryResult> res{ WorldDatabase.Query(
            "SELECT `list`, `name`, `map`, `x`, `y`, `z`, `o` FROM `dialog_dest` ORDER BY `list`, `id`") })
    {
        do
        {
            Field* f = res->Fetch();
            uint32 const list = f[0].GetUInt32();

            /*
             * A PLACE IN NO LIST IS DROPPED, LOUDLY.
             *
             * It cannot be reached -- the only way into this table is through a
             * list id -- so keeping it would mean carrying a row that is
             * invisible everywhere except in the database. `.dest` writes the
             * list first and refuses if that fails, so the only way to make one
             * is to delete a list by hand.
             */
            auto it = fresh->lists.find(list);
            if (it == fresh->lists.end())
            {
                ++orphans;
                continue;
            }

            DialogDestPoint p;
            p.name = f[1].GetCppString();
            p.map = f[2].GetUInt32();
            p.x = f[3].GetFloat();
            p.y = f[4].GetFloat();
            p.z = f[5].GetFloat();
            p.o = f[6].GetFloat();
            it->second.points.push_back(p);
        } while (res->NextRow());
    }

    uint32 places = 0;
    uint32 empty = 0;
    for (auto const& l : fresh->lists)
    {
        places += (uint32)l.second.points.size();
        if (l.second.points.empty())
            ++empty;
    }

    sLog.outString(">> Loaded %u teleport destination(s) in %u list(s).",
        places, (uint32)fresh->lists.size());

    /*
     * BOTH OF THESE ARE WARNINGS, NOT REFUSALS. An empty list is the ordinary
     * state of one somebody has just named and not yet filled, and the button
     * that names it fails with one line at press time rather than at load. It
     * is worth saying at startup anyway, because the alternative is finding out
     * by pressing.
     */
    if (empty)
        sLog.outString(">> %u destination list(s) are EMPTY -- a button using one does nothing.", empty);
    if (orphans)
        sLog.outErrorDb("Table `dialog_dest` has %u row(s) whose `list` is in no `dialog_dest_list`. Ignored.", orphans);

    std::lock_guard<std::mutex> guard(s_dataLock);
    s_data = fresh;
}
