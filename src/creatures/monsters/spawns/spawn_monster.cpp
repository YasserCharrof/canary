/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "pch.hpp"

#include "creatures/monsters/spawns/spawn_monster.hpp"
#include "game/game.hpp"
#include "creatures/monsters/monster.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "game/scheduling/events_scheduler.hpp"
#include "lua/creature/events.hpp"
#include "lua/callbacks/event_callback.hpp"
#include "lua/callbacks/events_callbacks.hpp"
#include "utils/pugicast.hpp"
#include "game/zones/zone.hpp"
#include "map/spectators.hpp"

static constexpr int32_t MONSTER_MINSPAWN_INTERVAL = 1000; // 1 second
static constexpr int32_t MONSTER_MAXSPAWN_INTERVAL = 86400000; // 1 day

bool SpawnsMonster::loadFromXML(const std::string &filemonstername) {
	if (isLoaded()) {
		return true;
	}

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filemonstername.c_str());
	if (!result) {
		printXMLError(__FUNCTION__, filemonstername, result);
		return false;
	}

	this->filemonstername = filemonstername;
	loaded = true;

	uint32_t eventschedule = g_eventsScheduler().getSpawnMonsterSchedule();
	std::string boostedNameGet = g_game().getBoostedMonsterName();

	for (auto spawnMonsterNode : doc.child("monsters").children()) {
		Position centerPos(
			pugi::cast<uint16_t>(spawnMonsterNode.attribute("centerx").value()),
			pugi::cast<uint16_t>(spawnMonsterNode.attribute("centery").value()),
			pugi::cast<uint16_t>(spawnMonsterNode.attribute("centerz").value())
		);

		int32_t radius;
		pugi::xml_attribute radiusAttribute = spawnMonsterNode.attribute("radius");
		if (radiusAttribute) {
			radius = pugi::cast<int32_t>(radiusAttribute.value());
		} else {
			radius = -1;
		}

		if (!spawnMonsterNode.first_child()) {
			g_logger().warn("[SpawnsMonster::loadFromXml] - Empty spawn at position: {} with radius: {}", centerPos.toString(), radius);
			continue;
		}

		spawnMonsterList.emplace_front(centerPos, radius);
		SpawnMonster &spawnMonster = spawnMonsterList.front();

		for (auto childMonsterNode : spawnMonsterNode.children()) {
			if (strcasecmp(childMonsterNode.name(), "monster") == 0) {
				pugi::xml_attribute nameAttribute = childMonsterNode.attribute("name");
				if (!nameAttribute) {
					continue;
				}

				Direction dir;

				pugi::xml_attribute directionAttribute = childMonsterNode.attribute("direction");
				if (directionAttribute) {
					dir = static_cast<Direction>(pugi::cast<uint16_t>(directionAttribute.value()));
				} else {
					dir = DIRECTION_NORTH;
				}

				auto xOffset = pugi::cast<int16_t>(childMonsterNode.attribute("x").value());
				auto yOffset = pugi::cast<int16_t>(childMonsterNode.attribute("y").value());
				Position pos(
					static_cast<uint16_t>(centerPos.x + xOffset),
					static_cast<uint16_t>(centerPos.y + yOffset),
					centerPos.z
				);

				int32_t boostedrate = 1;

				if (nameAttribute.value() == boostedNameGet) {
					boostedrate = 2;
				}

				pugi::xml_attribute weightAttribute = childMonsterNode.attribute("weight");
				uint32_t weight = 1;
				if (weightAttribute) {
					weight = pugi::cast<uint32_t>(weightAttribute.value());
				}

				uint32_t interval = pugi::cast<uint32_t>(childMonsterNode.attribute("spawntime").value()) * 1000 * 100 / std::max((uint32_t)1, (g_configManager().getNumber(RATE_SPAWN) * boostedrate * eventschedule));
				if (interval >= MONSTER_MINSPAWN_INTERVAL && interval <= MONSTER_MAXSPAWN_INTERVAL) {
					spawnMonster.addMonster(nameAttribute.as_string(), pos, dir, static_cast<uint32_t>(interval), weight);
				} else {
					if (interval <= MONSTER_MINSPAWN_INTERVAL) {
						g_logger().warn("[SpawnsMonster::loadFromXml] - {} {} spawntime cannot be less than {} seconds, set to {} by default.", nameAttribute.as_string(), pos.toString(), MONSTER_MINSPAWN_INTERVAL / 1000, MONSTER_MINSPAWN_INTERVAL / 1000);
						spawnMonster.addMonster(nameAttribute.as_string(), pos, dir, MONSTER_MINSPAWN_INTERVAL, weight);
					} else {
						g_logger().warn("[SpawnsMonster::loadFromXml] - {} {} spawntime can not be more than {} seconds", nameAttribute.as_string(), pos.toString(), MONSTER_MAXSPAWN_INTERVAL / 1000);
					}
				}
			}
		}
	}
	return true;
}

void SpawnsMonster::startup() {
	if (!isLoaded() || isStarted()) {
		return;
	}

	for (SpawnMonster &spawnMonster : spawnMonsterList) {
		spawnMonster.startup();
	}

	started = true;
}

void SpawnsMonster::clear() {
	for (SpawnMonster &spawnMonster : spawnMonsterList) {
		spawnMonster.stopEvent();
	}
	spawnMonsterList.clear();

	loaded = false;
	started = false;
	filemonstername.clear();
}

bool SpawnsMonster::isInZone(const Position &centerPos, int32_t radius, const Position &pos) {
	if (radius == -1) {
		return true;
	}

	return ((pos.getX() >= centerPos.getX() - radius) && (pos.getX() <= centerPos.getX() + radius) && (pos.getY() >= centerPos.getY() - radius) && (pos.getY() <= centerPos.getY() + radius));
}

void SpawnMonster::startSpawnMonsterCheck() {
	if (checkSpawnMonsterEvent == 0) {
		checkSpawnMonsterEvent = g_dispatcher().scheduleEvent(getInterval(), std::bind(&SpawnMonster::checkSpawnMonster, this), "SpawnMonster::checkSpawnMonster");
	}
}

SpawnMonster::~SpawnMonster() {
	for (const auto &[_, monster] : spawnedMonsterMap) {
		monster->setSpawnMonster(nullptr);
	}
}

bool SpawnMonster::findPlayer(const Position &pos) {
	auto spectators = Spectators().find<Player>(pos);
	for (const auto &spectator : spectators) {
		if (!spectator->getPlayer()->hasFlag(PlayerFlags_t::IgnoredByMonsters)) {
			return true;
		}
	}
	return false;
}

bool SpawnMonster::isInSpawnMonsterZone(const Position &pos) {
	return SpawnsMonster::isInZone(centerPos, radius, pos);
}

bool SpawnMonster::spawnMonster(uint32_t spawnMonsterId, const std::shared_ptr<MonsterType> monsterType, const Position &pos, Direction dir, bool startup /*= false*/) {
	if (spawnedMonsterMap.contains(spawnMonsterId)) {
		return false;
	}
	auto monster = std::make_shared<Monster>(monsterType);
	if (startup) {
		// No need to send out events to the surrounding since there is no one out there to listen!
		if (!g_game().internalPlaceCreature(monster, pos, true)) {
			return false;
		}
	} else {
		if (!g_game().placeCreature(monster, pos, false, true)) {
			return false;
		}
	}

	monster->setDirection(dir);
	monster->setSpawnMonster(this);
	monster->setMasterPos(pos);

	spawnedMonsterMap[spawnMonsterId] = monster;
	spawnMonsterMap[spawnMonsterId].lastSpawn = OTSYS_TIME();
	g_events().eventMonsterOnSpawn(monster, pos);
	g_callbacks().executeCallback(EventCallback_t::monsterOnSpawn, &EventCallback::monsterOnSpawn, monster, pos);
	return true;
}

void SpawnMonster::startup(bool delayed) {
	if (g_configManager().getBoolean(RANDOM_MONSTER_SPAWN)) {
		for (auto it = spawnMonsterMap.begin(); it != spawnMonsterMap.end(); ++it) {
			auto &[spawnMonsterId, sb] = *it;
			for (auto &[monsterType, weight] : sb.monsterTypes) {
				if (monsterType->isBoss()) {
					continue;
				}
				for (auto otherIt = std::next(it); otherIt != spawnMonsterMap.end(); ++otherIt) {
					auto &[id, otherSb] = *otherIt;
					if (id == spawnMonsterId) {
						continue;
					}
					if (otherSb.hasBoss()) {
						continue;
					}
					if (otherSb.monsterTypes.contains(monsterType)) {
						weight += otherSb.monsterTypes[monsterType];
					}
					otherSb.monsterTypes.emplace(monsterType, weight);
					sb.monsterTypes.emplace(monsterType, weight);
				}
			}
		}
	}
	for (auto &[spawnMonsterId, sb] : spawnMonsterMap) {
		const auto &mType = sb.getMonsterType();
		if (!mType) {
			continue;
		}
		if (delayed) {
			g_dispatcher().addEvent(std::bind(&SpawnMonster::scheduleSpawn, this, spawnMonsterId, sb, mType, 0), "SpawnMonster::startup");
		} else {
			scheduleSpawn(spawnMonsterId, sb, mType, 0);
		}
	}
}

void SpawnMonster::checkSpawnMonster() {
	if (checkSpawnMonsterEvent == 0) {
		return;
	}

	checkSpawnMonsterEvent = 0;
	cleanup();
	uint32_t spawnMonsterCount = 0;

	for (auto &[spawnMonsterId, sb] : spawnMonsterMap) {
		if (spawnedMonsterMap.contains(spawnMonsterId)) {
			continue;
		}

		const auto &mType = sb.getMonsterType();
		if (!mType) {
			continue;
		}
		if (!mType->canSpawn(sb.pos)) {
			sb.lastSpawn = OTSYS_TIME();
			continue;
		}
		if (mType->info.isBlockable && findPlayer(sb.pos)) {
			sb.lastSpawn = OTSYS_TIME();
			continue;
		}
		if (OTSYS_TIME() < sb.lastSpawn + sb.interval) {
			continue;
		}

		if (mType->info.isBlockable) {
			spawnMonster(spawnMonsterId, mType, sb.pos, sb.direction, true);
		} else {
			scheduleSpawn(spawnMonsterId, sb, mType, 3 * NONBLOCKABLE_SPAWN_MONSTER_INTERVAL);
		}

		if (++spawnMonsterCount >= static_cast<uint32_t>(g_configManager().getNumber(RATE_SPAWN))) {
			break;
		}
	}

	if (spawnedMonsterMap.size() < spawnMonsterMap.size()) {
		checkSpawnMonsterEvent = g_dispatcher().scheduleEvent(getInterval(), std::bind(&SpawnMonster::checkSpawnMonster, this), "SpawnMonster::checkSpawnMonster");
	}
}

void SpawnMonster::scheduleSpawn(uint32_t spawnMonsterId, spawnBlock_t &sb, const std::shared_ptr<MonsterType> mType, uint16_t interval) {
	if (interval <= 0) {
		spawnMonster(spawnMonsterId, mType, sb.pos, sb.direction);
	} else {
		g_game().addMagicEffect(sb.pos, CONST_ME_TELEPORT);
		g_dispatcher().scheduleEvent(1400, std::bind(&SpawnMonster::scheduleSpawn, this, spawnMonsterId, sb, mType, interval - NONBLOCKABLE_SPAWN_MONSTER_INTERVAL), "SpawnMonster::scheduleSpawn");
	}
}

void SpawnMonster::cleanup() {
	std::vector<uint32_t> removeList;
	for (const auto &[spawnMonsterId, monster] : spawnedMonsterMap) {
		if (monster == nullptr || monster->isRemoved()) {
			removeList.push_back(spawnMonsterId);
		}
	}
	for (const auto &spawnMonsterId : removeList) {
		spawnedMonsterMap.erase(spawnMonsterId);
	}
}

bool SpawnMonster::addMonster(const std::string &name, const Position &pos, Direction dir, uint32_t scheduleInterval, uint32_t weight /*= 1*/) {
	std::string variant = "";
	for (const auto &zone : Zone::getZones(pos)) {
		if (!zone->getMonsterVariant().empty()) {
			variant = zone->getMonsterVariant() + "|";
			break;
		}
	}
	const auto monsterType = g_monsters().getMonsterType(variant + name);
	if (!monsterType) {
		g_logger().error("Can not find {}", name);
		return false;
	}

	this->interval = std::min(this->interval, scheduleInterval);

	spawnBlock_t* sb = nullptr;
	uint32_t spawnMonsterId = spawnMonsterMap.size() + 1;
	for (auto &[id, maybeSb] : spawnMonsterMap) {
		if (maybeSb.pos == pos) {
			sb = &maybeSb;
			spawnMonsterId = id;
			break;
		}
	}
	if (sb) {
		if (sb->monsterTypes.contains(monsterType)) {
			g_logger().error("[SpawnMonster] Monster {} already exists in spawn block at {}", name, pos.toString());
			return false;
		}
		if (monsterType->isBoss() && sb->monsterTypes.size() > 0) {
			g_logger().error("[SpawnMonster] Boss monster {} has been added to spawn block with other monsters. This is not allowed.", name);
			return false;
		}
		if (sb->hasBoss()) {
			g_logger().error("[SpawnMonster] Monster {} has been added to spawn block with a boss. This is not allowed.", name);
			return false;
		}
	}
	if (!sb) {
		sb = &spawnMonsterMap.emplace(spawnMonsterId, spawnBlock_t()).first->second;
	}
	sb->monsterTypes.emplace(monsterType, weight);
	sb->pos = pos;
	sb->direction = dir;
	sb->interval = scheduleInterval;
	sb->lastSpawn = 0;
	return true;
}

void SpawnMonster::removeMonster(std::shared_ptr<Monster> monster) {
	uint32_t spawnMonsterId = 0;
	for (const auto &[id, m] : spawnedMonsterMap) {
		if (m == monster) {
			spawnMonsterId = id;
			break;
		}
	}
	spawnedMonsterMap.erase(spawnMonsterId);
}

void SpawnMonster::setMonsterVariant(const std::string &variant) {
	for (auto &it : spawnMonsterMap) {
		std::unordered_map<std::shared_ptr<MonsterType>, uint32_t> monsterTypes;
		for (const auto &[monsterType, weight] : it.second.monsterTypes) {
			auto variantName = variant + monsterType->typeName;
			auto variantType = g_monsters().getMonsterType(variantName, false);
			monsterTypes.emplace(variantType, weight);
		}
		it.second.monsterTypes = monsterTypes;
	}
}

void SpawnMonster::stopEvent() {
	if (checkSpawnMonsterEvent != 0) {
		g_dispatcher().stopEvent(checkSpawnMonsterEvent);
		checkSpawnMonsterEvent = 0;
	}
}

std::shared_ptr<MonsterType> spawnBlock_t::getMonsterType() const {
	if (monsterTypes.empty()) {
		return nullptr;
	}
	uint32_t totalWeight = 0;
	for (const auto &[mType, weight] : monsterTypes) {
		if (!mType) {
			continue;
		}
		if (mType->isBoss()) {
			if (monsterTypes.size() > 1) {
				g_logger().warn("[SpawnMonster] Boss monster {} has been added to spawn block with other monsters. This is not allowed.", mType->name);
			}
			return mType;
		}
		totalWeight += weight;
	}
	uint32_t randomWeight = uniform_random(0, totalWeight - 1);
	// order monsters by weight DESC
	std::vector<std::pair<std::shared_ptr<MonsterType>, uint32_t>> orderedMonsterTypes(monsterTypes.begin(), monsterTypes.end());
	std::sort(orderedMonsterTypes.begin(), orderedMonsterTypes.end(), [](const auto &a, const auto &b) {
		return a.second > b.second;
	});
	for (const auto &[mType, weight] : orderedMonsterTypes) {
		if (randomWeight < weight) {
			return mType;
		}
		randomWeight -= weight;
	}
	return nullptr;
}

bool spawnBlock_t::hasBoss() const {
	for (const auto &[monsterType, weight] : monsterTypes) {
		if (monsterType->isBoss()) {
			return true;
		}
	}
	return false;
}
