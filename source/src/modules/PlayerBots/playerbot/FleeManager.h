#pragma once

#include "WorldPosition.h"

class Player;

namespace ai
{
    class Engine;

    // Hostile creatures in sight of the bot that are not in a fight yet.
    std::list<Unit*> IdleHostiles(PlayerbotAI* ai);
    // A point inside the aggro radius of one of them, plus margin, pulls a new pack.
    // A mob more than 10 yd above or below the point is on another floor and is skipped.
    bool InIdleAggro(const std::list<Unit*>& idle, Player* bot, float x, float y, float z, float margin = 3.0f);

    // A spot for a companion on a ring around its master, on the side it already
    // stands. False when none passes; `note` says where.
    bool FindMasterRingSpot(PlayerbotAI* ai, float& rx, float& ry, float& rz, std::string& note);

	

	class FleePoint {
	public:
        FleePoint(PlayerbotAI* ai, float x, float y, float z) : ai(ai), sumDistance(0.0f), minDistance(0.0f) {
			this->x = x;
			this->y = y;
			this->z = z;
		}

	public:
		float x;
		float y;
		float z;

        float sumDistance;
        float minDistance;

    private:
        PlayerbotAI* ai;
	};

	class FleeManager
	{
	public:
        FleeManager(Player* bot, float maxAllowedDistance, float followAngle, bool forceMaxDistance = false, WorldPosition startPosition = WorldPosition()) {
			this->bot = bot;
			this->maxAllowedDistance = maxAllowedDistance;
			this->followAngle = followAngle;
            this->forceMaxDistance = forceMaxDistance;
			this->startPosition = startPosition ? startPosition : WorldPosition(bot);
		}

	public:
		bool CalculateDestination(float* rx, float* ry, float* rz);
        bool isUseful();

        // For a flee in a fight: skip a point more than 3 yd above or below the bot (the
        // top of a crate, another floor), and walk the path to the best points before one
        // is chosen. Off for the random bot teleport, which searches up to sight distance
        // over open hills.
        void CheckGround() { checkGround = true; }

	private:
		void calculatePossibleDestinations(std::list<FleePoint*> &points);
		void calculateDistanceToCreatures(FleePoint *point);
		void cleanup(std::list<FleePoint*> &points);
		FleePoint* selectOptimalDestination(std::list<FleePoint*> &points);
		bool isBetterThan(FleePoint* point, FleePoint* other);
		bool isTooCloseToEdge(float x, float y, float z, float angle);
        bool hasShortPath(FleePoint* point);

	private:
		Player* bot;
		float maxAllowedDistance;
		float followAngle;
        bool forceMaxDistance;
        bool checkGround = false;
		WorldPosition startPosition;
	};

};
