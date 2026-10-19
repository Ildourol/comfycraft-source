
#include "playerbot.h"
#include "FleeManager.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "Group/Group.h"
#include "strategy/values/MoveStyleValue.h"
#include "playerbot/ServerFacade.h"
#include "Maps/PathFinder.h"
#include <iomanip>
#include <sstream>

using namespace ai;

std::list<Unit*> ai::IdleHostiles(PlayerbotAI* ai)
{
    std::list<Unit*> idle;
    Player* bot = ai->GetBot();
    std::list<ObjectGuid> units = *ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("possible targets no los");
    for (ObjectGuid const& guid : units)
    {
        Unit* unit = ai->GetUnit(guid);
        if (unit && unit->GetTypeId() == TYPEID_UNIT && sServerFacade.IsAlive(unit) && !sServerFacade.IsInCombat(unit) && unit->IsHostileTo(bot))
            idle.push_back(unit);
    }

    return idle;
}

bool ai::InIdleAggro(const std::list<Unit*>& idle, Player* bot, float x, float y, float z, float margin)
{
    for (Unit* unit : idle)
    {
        if (std::fabs(unit->GetPositionZ() - z) > 10.0f)
            continue;

        if (sServerFacade.GetDistance2d(unit, x, y) < unit->GetAttackDistance(bot) + margin)
            return true;
    }

    return false;
}

void FleeManager::calculateDistanceToCreatures(FleePoint *point)
{
    point->minDistance = -1.0f;
    point->sumDistance = 0.0f;
    std::list<ObjectGuid> units = *GetBotAI(bot)->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("possible targets no los");
	for (std::list<ObjectGuid>::iterator i = units.begin(); i != units.end(); ++i)
    {
		Unit* unit = GetBotAI(bot)->GetUnit(*i);
		if (!unit)
		    continue;

        // do not count non-LOS mobs
        if (!unit->IsWithinLOS(point->x, point->y, point->z + unit->GetCollisionHeight(), true))
            continue;

		float d = sServerFacade.GetDistance2d(unit, point->x, point->y);
        point->sumDistance += d;
        if (point->minDistance < 0 || point->minDistance > d) point->minDistance = d;
	}
}

// The smallest angle between two orientations, 0 to pi. The ring loop runs past 2 pi,
// so a plain difference missed a mob near orientation 0.
static float angleBetween(float a, float b)
{
    float d = std::fmod(std::fabs(a - b), 2 * M_PI_F);
    return d > M_PI_F ? 2 * M_PI_F - d : d;
}

// A SPOT AROUND THE PLAYER, NOT AROUND THE BOT (2026-10-19, with the owner).
//
// Flee searched rings around the bot's own position, so two flees in a row walked
// it farther and farther out, and two companions starting side by side picked
// the same best point. Now a companion led by a player takes a spot on a ring
// around the player, 12 or 18 yd out, starting from the side it already stands
// on and working round both ways. The far side from the fight was tried first
// and was wrong: a tank runs past the mob and turns, and the far side is then
// deeper into the dungeon. The checks below move it off a bad spot, and patrol
// watch moves it when a patrol comes. A spot within 5 yd of another group
// member is skipped, so companions do not stack.
//
// A spot must be clear of idle mobs and patrols (aggro radius + 5 yd), at least
// 10 yd from every mob in the fight, within 3 yd of the player's height, in the
// player's sight, with a path at most 1.5 times the straight line + 5 yd, and
// for a hunter 10 to 30 yd from his target, measured as the server does.
bool ai::FindMasterRingSpot(PlayerbotAI* ai, float& rx, float& ry, float& rz, std::string& note)
{
    Player* bot = ai->GetBot();
    Player* master = ai->GetMaster();
    if (!master || master == bot || !master->IsInWorld() || master->GetMapId() != bot->GetMapId())
        return false;

    const std::list<Unit*> idle = IdleHostiles(ai);

    std::list<Unit*> fighting;
    for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get())
        if (Unit* unit = ai->GetUnit(guid))
            if (sServerFacade.IsAlive(unit))
                fighting.push_back(unit);

    // Other group members to keep 5 yd from.
    std::list<Player*> others;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member && member != master && member != bot && member->IsInWorld() && member->GetMapId() == bot->GetMapId())
                others.push_back(member);
        }
    }

    Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    const bool needsShot = bot->getClass() == CLASS_HUNTER && target && sServerFacade.IsAlive(target);

    // From the bot's own bearing, then 15 degrees left and right in turn, all the way round.
    const float step = M_PI_F / 12;
    std::vector<float> offsets = { 0.0f };
    for (int i = 1; i <= 12; ++i)
    {
        offsets.push_back(i * step);
        if (i < 12)
            offsets.push_back(-i * step);
    }

    const float own = atan2(bot->GetPositionY() - master->GetPositionY(), bot->GetPositionX() - master->GetPositionX());
    for (float offset : offsets)
    {
        for (float radius : { 12.0f, 18.0f })
        {
            const float angle = own + offset;
            float x = master->GetPositionX() + cos(angle) * radius;
            float y = master->GetPositionY() + sin(angle) * radius;
            float z = master->GetPositionZ() + CONTACT_DISTANCE;
            bot->UpdateAllowedPositionZ(x, y, z);

            if (std::fabs(z - master->GetPositionZ()) > 3.0f)
                continue;

            if (InIdleAggro(idle, bot, x, y, z, 5.0f))
                continue;

            bool crowded = false;
            for (Unit* unit : fighting)
                crowded = crowded || sServerFacade.GetDistance2d(unit, x, y) < 10.0f;
            for (Player* other : others)
                crowded = crowded || sServerFacade.GetDistance2d(other, x, y) < 5.0f;
            if (crowded)
                continue;

            if (needsShot)
            {
                const float shot = sServerFacade.GetDistance2d(target, x, y) - target->GetCombatReach() - bot->GetCombatReach();
                if (shot < 10.0f || shot > 30.0f)
                    continue;
            }

            if (!master->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
                continue;

            PathFinder path(bot);
            path.calculate(x, y, z, false);
            if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                continue;
            if ((path.getPathType() & PATHFIND_NORMAL) && path.Length() > 1.5f * bot->GetDistance(x, y, z) + 5.0f)
                continue;

            std::ostringstream out;
            out << std::fixed << std::setprecision(0) << radius << " yd from " << master->GetName() << ", "
                << (offset * 180.0f / M_PI_F) << " deg round from its own side";
            note = out.str();
            rx = x;
            ry = y;
            rz = z;
            return true;
        }
    }

    return false;
}

bool intersectsOri(float angle, std::list<float>& angles, float angleIncrement)
{
    for (std::list<float>::iterator i = angles.begin(); i != angles.end(); ++i)
    {
        float ori = *i;
        if (angleBetween(angle, ori) < angleIncrement) return true;
    }

    return false;
}

void FleeManager::calculatePossibleDestinations(std::list<FleePoint*> &points)
{
    Unit *target = *GetBotAI(bot)->GetAiObjectContext()->GetValue<Unit*>("current target");

    float botPosX = startPosition.getX();
    float botPosY = startPosition.getY();
    float botPosZ = startPosition.getZ();
    
    FleePoint start(GetBotAI(bot), botPosX, botPosY, botPosZ);
    calculateDistanceToCreatures(&start);

    std::list<float> enemyOri;
    // Hostile mobs not yet in the fight. A point inside one's aggro radius pulls a new pack.
    const std::list<Unit*> idle = IdleHostiles(GetBotAI(bot));
    std::list<ObjectGuid> units = *GetBotAI(bot)->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("possible targets no los");
    for (std::list<ObjectGuid>::iterator i = units.begin(); i != units.end(); ++i)
    {
        Unit* unit = GetBotAI(bot)->GetUnit(*i);
        if (!unit)
            continue;

        float ori = bot->GetAngle(unit);
        enemyOri.push_back(ori);
    }

    // Never flee past the current target: a point more than 90 degrees from straight away
    // is skipped. A bot with no such point fights where it stands.
    const bool awayFromTarget = target && sServerFacade.IsAlive(target) && sServerFacade.IsAlive(bot);
    const float targetOri = awayFromTarget ? atan2(target->GetPositionY() - botPosY, target->GetPositionX() - botPosX) : 0.0f;

    float distIncrement = std::max(sPlayerbotAIConfig.followDistance, (maxAllowedDistance - sPlayerbotAIConfig.tooCloseDistance) / 10.0f);
    for (float dist = maxAllowedDistance; dist >= sPlayerbotAIConfig.tooCloseDistance; dist -= distIncrement)
    {
        float angleIncrement = std::max(M_PI / 20, M_PI / 4 / (1.0 + dist - sPlayerbotAIConfig.tooCloseDistance));
        for (float add = 0.0f; add < M_PI / 4 + angleIncrement; add += angleIncrement)
        {
            for (float angle = add; angle < add + 2 * M_PI_F + angleIncrement; angle += M_PI_F / 4)
            {
                if (intersectsOri(angle, enemyOri, angleIncrement)) continue;
                if (awayFromTarget && angleBetween(angle, targetOri) < M_PI_F / 2) continue;

                // Each ring at its own distance; all of them used maxAllowedDistance, so a tight
                // place where the outer ring meets walls had no point at all.
                float x = botPosX + cos(angle) * dist, y = botPosY + sin(angle) * dist, z = botPosZ + CONTACT_DISTANCE;
                if (MoveStyleValue::CheckForEdges(GetBotAI(bot)) && isTooCloseToEdge(x, y, z, angle)) continue;

                if (forceMaxDistance && sServerFacade.IsDistanceLessThan(sServerFacade.GetDistance2d(bot, x, y), maxAllowedDistance - sPlayerbotAIConfig.tooCloseDistance))
                    continue;

                bot->UpdateAllowedPositionZ(x, y, z);

                if (checkGround && std::fabs(z - botPosZ) > 3.0f)
                    continue;

                if (InIdleAggro(idle, bot, x, y, z))
                    continue;

                const TerrainInfo* terrain = startPosition.getTerrain();
                if (terrain && terrain->IsInWater(x, y, z))
                    continue;

                if (/*!bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true) || */(target && !target->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true)))
                    continue;

                FleePoint *point = new FleePoint(GetBotAI(bot), x, y, z);
                calculateDistanceToCreatures(point);

                if (sServerFacade.IsDistanceGreaterOrEqualThan(point->minDistance - start.minDistance, sPlayerbotAIConfig.followDistance))
                    points.push_back(point);
                else
                    delete point;
            }
        }
	}
}

bool FleeManager::isTooCloseToEdge(float x, float y, float z, float angle)
{
    Map* map = bot->GetMap();
    const TerrainInfo* terrain = map->GetTerrain();
    for (double a = angle; a <= angle + 2*M_PI; a += M_PI / 4)
    {
        float dist = sPlayerbotAIConfig.followDistance;
        float tx = x + cos(a) * dist;
        float ty = y + sin(a) * dist;
        float tz = z;
        bot->UpdateAllowedPositionZ(tx, ty, tz);

        if (terrain && terrain->IsInWater(tx, ty, tz))
            return true;

        if (!bot->IsWithinLOS(tx, ty, tz))
            return true;
    }

    return false;
}

void FleeManager::cleanup(std::list<FleePoint*> &points)
{
	for (std::list<FleePoint*>::iterator i = points.begin(); i != points.end(); i++)
    {
		FleePoint* point = *i;
		delete point;
	}
	points.clear();
}

bool FleeManager::isBetterThan(FleePoint* point, FleePoint* other)
{
    return point->sumDistance - other->sumDistance > 0;
}

FleePoint* FleeManager::selectOptimalDestination(std::list<FleePoint*> &points)
{
	FleePoint* best = NULL;
	for (std::list<FleePoint*>::iterator i = points.begin(); i != points.end(); i++)
    {
		FleePoint* point = *i;
        if (!best || isBetterThan(point, best))
            best = point;
	}

	return best;
}

// A walkable path no longer than 1.5 times the straight line. A longer one goes
// round by a ramp or another floor.
bool FleeManager::hasShortPath(FleePoint* point)
{
    PathFinder path(bot);
    path.calculate(point->x, point->y, point->z, false);
    const uint32 type = path.getPathType();
    if (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
        return false;

    // No navmesh here: nothing to compare against.
    if (!(type & PATHFIND_NORMAL))
        return true;

    return path.Length() <= 1.5f * bot->GetDistance(point->x, point->y, point->z);
}

bool FleeManager::CalculateDestination(float* rx, float* ry, float* rz)
{
    std::list<FleePoint*> points;
	calculatePossibleDestinations(points);

    FleePoint* point = nullptr;
    if (checkGround)
    {
        // A path for every candidate costs too much; walk the best five.
        points.sort([](FleePoint* a, FleePoint* b) { return a->sumDistance > b->sumDistance; });
        int tried = 0;
        for (FleePoint* candidate : points)
        {
            if (tried++ == 5)
                break;

            if (hasShortPath(candidate))
            {
                point = candidate;
                break;
            }
        }
    }
    else
        point = selectOptimalDestination(points);
    if (!point)
    {
        cleanup(points);
        return false;
    }

	*rx = point->x;
	*ry = point->y;
	*rz = point->z;

    cleanup(points);
	return true;
}

bool FleeManager::isUseful()
{
    // It the bot is a victim of an aoe attack it should move no matter the target attack distance
    bool const inAoe = GetBotAI(bot)->GetAiObjectContext()->GetValue<bool>("has area debuff", "self target")->Get();
    if (!inAoe)
    {
        std::list<ObjectGuid> units = *GetBotAI(bot)->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("possible targets no los");
        for (std::list<ObjectGuid>::iterator i = units.begin(); i != units.end(); ++i)
        {
            Unit* unit = GetBotAI(bot)->GetUnit(*i);
            if (unit)
            {
                float const distanceSquared = startPosition.sqDistance(WorldPosition(unit));
                float attackDistanceSquared = unit->GetAttackDistance(bot);
                attackDistanceSquared *= attackDistanceSquared;
                if (distanceSquared < attackDistanceSquared)
                {
                    return true;
                }

                //float d = sServerFacade.GetDistance2d(unit, bot);
                //if (sServerFacade.IsDistanceLessThan(d, sPlayerbotAIConfig.aggroDistance)) return true;
            }
        }

        return false;
    }

    return true;
}
