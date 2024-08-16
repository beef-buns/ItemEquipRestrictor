#include "Manager.h"
#include "Settings.h"

namespace ItemRestrictor
{
	void Manager::Register()
	{
		if (const auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton()) {
			scriptEventSourceHolder->AddEventSink<RE::TESEquipEvent>(GetSingleton());
			scriptEventSourceHolder->AddEventSink<RE::TESObjectLoadedEvent>(GetSingleton());
			scriptEventSourceHolder->AddEventSink<RE::TESSwitchRaceCompleteEvent>(GetSingleton());
		}
	}

	void Manager::AddAnimationEvent(const RE::Actor* a_actor)
	{
		a_actor->AddAnimationGraphEventSink(GetSingleton());
	}

	void Manager::RemoveAnimationEvent(const RE::Actor* a_actor)
	{
		a_actor->RemoveAnimationGraphEventSink(GetSingleton());
	}

	void Manager::get_npc_edids(RE::Actor* a_actor, const RE::TESNPC* a_npc, std::vector<std::string>& a_edids)
	{
		if (const auto extraLvlCreature = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>()) {
			if (const auto originalBase = extraLvlCreature->originalBase) {
				a_edids.emplace_back(edid::get_editorID(originalBase));
			}
			if (const auto templateBase = extraLvlCreature->templateBase) {
				a_edids.emplace_back(edid::get_editorID(templateBase));
			}
		} else {
			a_edids.emplace_back(edid::get_editorID(a_npc));
		}
	}

	bool Manager::is_bow_or_crossbow(RE::TESForm* a_object)
	{
		if (const auto weap = a_object->As<RE::TESObjectWEAP>(); weap && (weap->IsBow() || weap->IsCrossbow())) {
			return true;
		}
		return false;
	}

	std::pair<bool, RE::TESForm*> Manager::ShouldSkip(const std::string& a_keywordEDID, RE::Actor* a_actor, const RE::TESNPC* a_npc, const RE::TESBoundObject* a_object, RestrictParams& a_params)
	{
		if (a_params.restrictOn == RESTRICT_ON::kEquip && !a_keywordEDID.starts_with("RestrictEquip:") || a_params.restrictOn == RESTRICT_ON::kCast && !a_keywordEDID.starts_with("RestrictCast:")) {
			return { false, nullptr };
		}

		const auto restrict_kywd = string::split(a_keywordEDID, ":");
		const auto isPlayer = a_actor->IsPlayerRef();

		// contains debuff perk param
		if (a_params.restrictOn == RESTRICT_ON::kEquip && a_params.restrictType == RESTRICT_TYPE::kRestrict && restrict_kywd.size() > 2 && isPlayer) {
			return { false, nullptr };
		}

		const auto sex = a_npc->GetSex();
		const auto actorLevel = a_npc->GetLevel();
		const auto lHand = a_actor->GetEquippedObject(true);
		const auto rHand = a_actor->GetEquippedObject(false);
		const auto isAmmo = a_object->IsAmmo();
		const auto isLHandBow = isAmmo && lHand && is_bow_or_crossbow(lHand);
		const auto isRHandBow = isAmmo && rHand && is_bow_or_crossbow(rHand);
		const auto inCombat = a_actor->IsInCombat();
		const auto inventory = a_actor->GetInventory();

		const auto match_keywords = [&](const std::string& a_filter) {
			if (a_actor->HasKeywordString(a_filter)) {
				return true;
			}
			if (isAmmo) {
				if (!isLHandBow && !isRHandBow) {
					return true;
				}
				return isRHandBow && rHand->HasKeywordByEditorID(a_filter) || isLHandBow && lHand->HasKeywordByEditorID(a_filter);
			}
			for (auto& [item, data] : inventory) {
				auto& [count, entry] = data;
				if (entry->IsWorn() && count > 0 && item->HasKeywordByEditorID(a_filter)) {
					return true;
				}
			}
			return false;
		};

		const auto match_filter = [&](const std::string& a_filter) {
			// RestrictEquip:Female -> men cannot wear this
			// RestrictEquip:!Female -> only men can wear this

			// RestrictEquip:ActorTypeVampire -> only vampires can wear this
			// RestrictEquip:!ActorTypeVampire -> vampires cannot wear this

			std::string filter_copy = a_filter;

			bool invert = false;
			if (filter_copy.starts_with('!')) {
				invert = true;
				filter_copy.erase(0, 1);
			}

			switch (string::const_hash(filter_copy)) {
			case "Male"_h:
				return invert ? sex == RE::SEX::kFemale : sex == RE::SEX::kMale;
			case "Female"_h:
				return invert ? sex == RE::SEX::kMale : sex == RE::SEX::kFemale;
			case "Player"_h:
				return invert ? !isPlayer : isPlayer;
			case "NPC"_h:
				return invert ? isPlayer : !isPlayer;
			case "Combat"_h:
				return invert ? inCombat : !inCombat;
			default:
				{
					if (filter_copy.starts_with("Level(")) {
						static srell::regex pattern(R"(\(([^)]+)\))");
						if (srell::smatch matches; srell::regex_search(filter_copy, matches, pattern)) {
							std::uint16_t level;
							if (string::is_only_digit(matches[1].str())) {
								level = string::to_num<std::uint16_t>(matches[1].str());
							} else {
								level = static_cast<std::uint16_t>(RE::TESForm::LookupByEditorID<RE::TESGlobal>(matches[1].str())->value);
							}
							if (const auto match = actorLevel >= level; invert ? !match : match) {
								return true;
							}
							a_params.restrictReason = RESTRICT_REASON::kLevel;
						}
						return false;
					}

					if (filter_copy.contains("(")) {
						static srell::regex pattern(R"(([^\(]*)\(([^)]+)\))");
						if (srell::smatch matches; srell::regex_search(filter_copy, matches, pattern)) {
							RE::ActorValue  av;
							float           minLevel;
							RE::TESFaction* faction = nullptr;
							logger::info("{}", matches[1].str());

							if (string::is_only_digit(matches[1].str())) {
								av = string::to_num<RE::ActorValue>(matches[1].str());
							} else if (const auto factionForm = RE::TESForm::LookupByEditorID<RE::TESFaction>(matches[1].str())) {
								logger::info("Faction exists");
								faction = factionForm;
							} else {
								av = RE::ActorValueList::GetSingleton()->LookupActorValueByName(matches[1].str());
							}
							if (string::is_only_digit(matches[2].str())) {
								minLevel = string::to_num<float>(matches[2].str());
							} else {
								minLevel = RE::TESForm::LookupByEditorID<RE::TESGlobal>(matches[2].str())->value;
							}

							if (faction != nullptr && a_actor->IsInFaction(faction)) {
								logger::info("In the faction");
								logger::info("minLevel: {}", minLevel);
								logger::info("faction rank: {}", a_actor->GetFactionRank(faction, isPlayer));
								if (const bool match = a_actor->GetFactionRank(faction, isPlayer) >= minLevel; invert ? !match : match) {
									logger::info("Returning true?");
									return true;
								}
							} else if (const bool match = a_actor->GetActorValue(av) >= minLevel; invert ? !match : match) {
								return true;
							}
							a_params.restrictReason = RESTRICT_REASON::kSkill;
						}
						return false;
					}

					bool match;

					if (const auto filterForm = RE::TESForm::LookupByEditorID(filter_copy)) {
						switch (filterForm->GetFormType()) {
						case RE::FormType::Faction:
							{
								match = a_actor->IsInFaction(filterForm->As<RE::TESFaction>());
								return invert ? !match : match;
							}
						case RE::FormType::Perk:
							{
								match = a_actor->HasPerk(filterForm->As<RE::BGSPerk>());
								return invert ? !match : match;
							}
						case RE::FormType::Race:
							{
								match = a_actor->GetRace() == filterForm;
								return invert ? !match : match;
							}
						case RE::FormType::Spell:
							{
								match = a_actor->HasSpell(filterForm->As<RE::SpellItem>());
								return invert ? !match : match;
							}
						case RE::FormType::MagicEffect:
							{
								match = a_actor->HasMagicEffect(filterForm->As<RE::EffectSetting>());
								return invert ? !match : match;
							}
						default:
							break;
						}
					}

					match = match_keywords(filter_copy);
					return invert ? !match : match;
				}
			}
		};

		const auto split_filters = string::split(restrict_kywd[1], ",");
		bool       shouldSkip = std::ranges::none_of(split_filters, [&](const std::string& a_filter) {
            if (a_filter.contains('+')) {
                const auto chained_filters = string::split(a_filter, "+");
                return std::ranges::all_of(chained_filters, match_filter);
            } else {
                return match_filter(a_filter);
            }
        });

		return { shouldSkip, restrict_kywd.size() > 2 ? RE::TESForm::LookupByEditorID(restrict_kywd[2]) : nullptr };
	}

	std::pair<bool, RE::TESForm*> Manager::ShouldSkip(RE::Actor* a_actor, RE::TESBoundObject* a_object, RestrictParams& a_params)
	{
		bool         skipEquip = false;
		RE::TESForm* debuffForm = nullptr;

		const auto base = a_actor->GetActorBase();
		if (!base) {
			return { skipEquip, debuffForm };
		}

		if (const auto keywordForm = a_object->As<RE::BGSKeywordForm>()) {
			keywordForm->ForEachKeyword([&](const RE::BGSKeyword* a_keyword) {
				if (const auto edid = a_keyword->GetFormEditorID(); !string::is_empty(edid)) {
					if (std::tie(skipEquip, debuffForm) = ShouldSkip(edid, a_actor, base, a_object, a_params); skipEquip) {
						return RE::BSContainer::ForEachResult::kStop;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		return { skipEquip, debuffForm };
	}

	void Manager::AddDebuff(const RE::TESBoundObject* a_item, RE::TESForm* a_debuffForm)
	{
		if (a_debuffForm->Is(RE::FormType::Perk)) {
			RE::PlayerCharacter::GetSingleton()->AddPerk(a_debuffForm->As<RE::BGSPerk>());
		} else {
			RE::PlayerCharacter::GetSingleton()->AddSpell(a_debuffForm->As<RE::SpellItem>());
		}

		_objectDebuffsMap[a_item->GetFormID()].insert(a_debuffForm->GetFormID());
		_debuffObjectsMap[a_debuffForm->GetFormID()].insert(a_item->GetFormID());
	}

	void Manager::RemoveDebuff(const RE::TESBoundObject* a_item)
	{
		const auto itemID = a_item->GetFormID();

		if (const auto oIt = _objectDebuffsMap.find(itemID); oIt != _objectDebuffsMap.end()) {
			for (const auto& debuffID : oIt->second) {
				if (const auto dIt = _debuffObjectsMap.find(debuffID); dIt != _debuffObjectsMap.end()) {
					if (dIt->second.erase(itemID) && dIt->second.empty()) {
						if (const auto debuffForm = RE::TESForm::LookupByID(debuffID)) {
							if (debuffForm->Is(RE::FormType::Perk)) {
								RE::PlayerCharacter::GetSingleton()->RemovePerk(debuffForm->As<RE::BGSPerk>());
							} else {
								RE::PlayerCharacter::GetSingleton()->RemoveSpell(debuffForm->As<RE::SpellItem>());
							}
						}
					}
				}
			}
			_objectDebuffsMap.erase(oIt);
		}
	}

	void Manager::ProcessShouldSkipCast(RE::Actor* a_actor, RE::MagicCaster* a_caster)
	{
		if (!a_caster || !a_caster->currentSpell) {
			return;
		}
		RestrictParams params{
			RESTRICT_ON::kCast,
			RESTRICT_TYPE::kRestrict,
			RESTRICT_REASON::kGeneric
		};
		if (auto [skipEquip, debuffForm] = ShouldSkip(a_actor, a_caster->currentSpell, params); skipEquip) {
			if (a_actor->IsPlayerRef()) {
				const auto notification = Settings::GetSingleton()->GetNotification(a_caster->currentSpell, params);
				if (!notification.empty()) {
					RE::DebugNotification(notification.c_str());
				}
			}
			a_caster->InterruptCast(true);
			RE::PlaySound("MAGFail");
		}
	}

	namespace Equip
	{
		struct DoEquip
		{
			static void thunk(RE::ActorEquipManager* a_manager, RE::Actor* a_actor, RE::TESBoundObject* a_object, const RE::ObjectEquipParams& a_objectEquipParams)
			{
				if (a_actor && a_object && !a_objectEquipParams.forceEquip) {
					if (!a_objectEquipParams.extraDataList || !a_objectEquipParams.extraDataList->HasQuestObjectAlias()) {
						RestrictParams params{
							RESTRICT_ON::kEquip,
							RESTRICT_TYPE::kRestrict,
							RESTRICT_REASON::kGeneric
						};
						if (auto [skipEquip, debuffForm] = Manager::ShouldSkip(a_actor, a_object, params); skipEquip) {
							if (a_actor->IsPlayerRef()) {
								const auto notification = Settings::GetSingleton()->GetNotification(a_object, params);
								if (a_objectEquipParams.showMessage && !notification.empty()) {
									RE::DebugNotification(notification.c_str());
								}
							}
							return;
						}
					}
				}

				return func(a_manager, a_actor, a_object, a_objectEquipParams);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Install()
		{
			std::array targets{
				std::make_pair(RELOCATION_ID(37938, 38894), OFFSET(0xE5, 0x170)),  //ActorEquipManager::EquipObject
				std::make_pair(RELOCATION_ID(37937, 38893), 0xBC),                 //ActorEquipManager::EquipImpl?
			};

			for (const auto& [id, offset] : targets) {
				REL::Relocation<std::uintptr_t> target{ id, offset };
				stl::write_thunk_call<DoEquip>(target.address());
			}
		}
	}

	RE::BSEventNotifyControl Manager::ProcessEvent(RE::TESEquipEvent const* a_evn, RE::BSTEventSource<RE::TESEquipEvent>*)
	{
		if (!a_evn || !a_evn->actor) {
			return RE::BSEventNotifyControl::kContinue;
		}

		const auto actor = a_evn->actor->As<RE::Actor>();
		if (!actor || !a_evn->actor->IsPlayerRef()) {
			return RE::BSEventNotifyControl::kContinue;
		}

		const auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(a_evn->baseObject);
		if (!item) {
			return RE::BSEventNotifyControl::kContinue;
		}

		if (a_evn->equipped) {
			RestrictParams params{ RESTRICT_ON::kEquip, RESTRICT_TYPE::kDebuff, RESTRICT_REASON::kGeneric };
			if (auto [skipEquip, debuffForm] = ShouldSkip(actor, item, params); skipEquip && debuffForm) {
				AddDebuff(item, debuffForm);
				const auto notification = Settings::GetSingleton()->GetNotification(item, params);
				if (!notification.empty()) {
					RE::DebugNotification(notification.c_str());
				}
			} else if (is_bow_or_crossbow(item)) {
				if (const auto ammo = actor->GetCurrentAmmo()) {
					params.restrictType = RESTRICT_TYPE::kRestrict;
					if (std::tie(skipEquip, debuffForm) = ShouldSkip(actor, ammo, params); skipEquip) {
						SKSE::GetTaskInterface()->AddTask([actor, ammo]() {
							RE::ActorEquipManager::GetSingleton()->UnequipObject(actor, ammo);
							RE::SendUIMessage::SendInventoryUpdateMessage(actor, nullptr);
						});
					}
				}
			}
		} else {
			RemoveDebuff(item);
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl Manager::ProcessEvent(RE::TESObjectLoadedEvent const* a_evn, RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
	{
		if (!a_evn) {
			return RE::BSEventNotifyControl::kContinue;
		}

		if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(a_evn->formID)) {
			AddAnimationEvent(actor);
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl Manager::ProcessEvent(RE::TESSwitchRaceCompleteEvent const* a_evn, RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*)
	{
		if (!a_evn || !a_evn->subject) {
			return RE::BSEventNotifyControl::kContinue;
		}

		if (const auto actor = a_evn->subject->As<RE::Actor>()) {
			AddAnimationEvent(actor);
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl Manager::ProcessEvent(RE::BSAnimationGraphEvent const* a_evn, RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
	{
		if (!a_evn || !a_evn->holder) {
			return RE::BSEventNotifyControl::kContinue;
		}

		const auto actor = const_cast<RE::Actor*>(a_evn->holder->As<RE::Actor>());
		if (!actor) {
			return RE::BSEventNotifyControl::kContinue;
		}

		switch (string::const_hash(a_evn->tag)) {
		case "BeginCastLeft"_h:
			ProcessShouldSkipCast(actor, actor->magicCasters[0]);
			break;
		case "BeginCastRight"_h:
			ProcessShouldSkipCast(actor, actor->magicCasters[1]);
			break;
		case "BeginCastVoice"_h:
			ProcessShouldSkipCast(actor, actor->magicCasters[3]);
			break;
		default:
			break;
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	void Install()
	{
		Settings::GetSingleton()->LoadSettings();
		Manager::Register();

		Equip::Install();
	}
}
