
#include "playerbot/playerbot.h"
#include "Trigger.h"
#include "Action.h"
#include "Objects/Unit.h"
#include "Value.h"

using namespace ai;

Event Trigger::Check()
{
	if (triggered)
	{
		Player* owner = nullptr;
		if (!FindOwner(owner))
			return Event();
		if (owner)
			return Event(getName(), param, owner);
		else
			return Event(getName());
	}

	if (IsActive())
	{
		triggered = true;
		Event event(getName());
		return event;
	}
	Event event;
	return event;
}

ObjectGuid Trigger::GuidOf(Player* player)
{
    return player ? player->GetObjectGuid() : ObjectGuid();
}

bool Trigger::FindOwner(Player*& owner)
{
    owner = nullptr;
    if (ownerGuid.IsEmpty())
        return true;

    owner = sObjectAccessor.FindPlayerNotInWorld(ownerGuid);
    if (!owner)
    {
        triggered = false;
        return false;
    }
    if (!owner->IsInWorld())
    {
        owner = nullptr;
        return false;
    }
    return true;
}

Value<Unit*>* Trigger::GetTargetValue()
{
    return context->GetValue<Unit*>(GetTargetName());
}

Unit* Trigger::GetTarget()
{
    return GetTargetValue()->Get();
}

TriggerNode::~TriggerNode()
{
	NextAction::destroy(handlers);
}

NextAction** TriggerNode::getHandlers()
{
	return NextAction::merge(NextAction::clone(handlers), trigger->getHandlers());
}

float TriggerNode::getFirstRelevance()
{
	return handlers[0] ? handlers[0]->getRelevance() : -1;
}
