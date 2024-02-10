#include "CollisionManager.h"


NameInfo::NameInfo(HashStringTableIndex NameIdx, ECollisionType CurrentType)
	: Name(NameIdx), CollisionData(0x0)
{
	// Member-Initializer order is not guaranteed, init "OwnType" after
	OwnType = static_cast<uint8>(CurrentType);
}

void NameInfo::InitCollisionData(const NameInfo& Existing, ECollisionType CurrentType, bool bIsSuper)
{
	CollisionData = Existing.CollisionData;
	OwnType = static_cast<uint8>(CurrentType);

	// Increments the CollisionCount, for the CollisionType specified, by 1
	const int32 ShiftCount = OwnTypeBitCount + ((Existing.OwnType + bIsSuper) * PerCountBitCount);
	const int32 IncrementForCollisionCount = 1 << ShiftCount;
	CollisionData += IncrementForCollisionCount;
}

bool NameInfo::HasCollisions() const
{
	ECollisionType OwnCollisionType = static_cast<ECollisionType>(OwnType);

	if (OwnCollisionType == ECollisionType::MemberName)
	{
		return SuperMemberNameCollisionCount > 0x0 || MemberNameCollisionCount > 0x0;
	}
	else if (OwnCollisionType == ECollisionType::FunctionName)
	{
		return MemberNameCollisionCount > 0x0 || SuperMemberNameCollisionCount > 0x0 || FunctionNameCollisionCount > 0x0;
	}
	else if (OwnCollisionType == ECollisionType::ParameterName)
	{
		return MemberNameCollisionCount > 0x0 || SuperMemberNameCollisionCount > 0x0 || FunctionNameCollisionCount > 0x0 || SuperFuncNameCollisionCount > 0x0 || ParamNameCollisionCount > 0x0;
	}

	return false;
}

uint64 KeyFunctions::GetKeyForCollisionInfo(UEStruct Super, UEProperty Member)
{
	uint64 Key = 0x0;

	FName Name = Member.GetFName();
	Key += Name.GetCompIdx();
	Key += Name.GetNumber();

	Key <<= 32;
	Key |= (static_cast<uint64>(Member.GetOffset()) << 24);
	Key |= (static_cast<uint64>(Member.GetSize()) << 16);
	Key |= Super.GetIndex();

	return reinterpret_cast<uint64>(Member.GetAddress());
}

uint64 KeyFunctions::GetKeyForCollisionInfo([[maybe_unused]] UEStruct Super, UEFunction Member)
{
	uint64 Key = 0x0;

	FName Name = Member.GetFName();
	Key += Name.GetCompIdx();
	Key += Name.GetNumber();

	Key <<= 32;
	Key |= Member.GetIndex();

	return Key;
}

uint64 CollisionManager::AddNameToContainer(NameContainer& StructNames, UEStruct Struct, std::pair<HashStringTableIndex, bool>&& NamePair, ECollisionType CurrentType, UEFunction Func)
{
	static auto AddCollidingName = [](const NameContainer& SearchNames, NameContainer* OutTargetNames, HashStringTableIndex NameIdx, ECollisionType CurrentType, bool bIsSuper) -> bool
	{
		assert(OutTargetNames && "Target container was nullptr!");

		NameInfo NewInfo(NameIdx, CurrentType);
		NewInfo.OwnType = static_cast<uint8>(CurrentType);

		for (auto RevIt = SearchNames.crbegin(); RevIt != SearchNames.crend(); ++RevIt)
		{
			const NameInfo& ExistingName = *RevIt;

			if (ExistingName.Name != NameIdx)
				continue;

			NewInfo.InitCollisionData(ExistingName, CurrentType, bIsSuper);
			OutTargetNames->push_back(NewInfo);

			return true;
		}

		return false;
	};

	const bool bIsParameter = CurrentType == ECollisionType::ParameterName;

	auto [NameIdx, bWasInserted] = NamePair;

	if (bWasInserted && !bIsParameter)
	{
		// Create new empty NameInfo
		StructNames.emplace_back(NameIdx, CurrentType);
		return StructNames.size() - 1;
	}

	NameContainer* FuncParamNames = nullptr;

	if (Func)
	{
		FuncParamNames = &NameInfos[Func.GetIndex()];

		if (bWasInserted && bIsParameter)
		{
			// Create new empty NameInfo
			FuncParamNames->emplace_back(NameIdx, CurrentType);
			return FuncParamNames->size() - 1;
		}

		if (AddCollidingName(*FuncParamNames, FuncParamNames, NameIdx, CurrentType, false))
			return FuncParamNames->size() - 1;

		/* Serach ReservedNames last, just in case there was a property which also collided with a reserved name already */
		if (AddCollidingName(ReservedNames, FuncParamNames, NameIdx, CurrentType, false))
			return FuncParamNames->size() - 1;
	}

	NameContainer* TargetNameContainer = bIsParameter ? FuncParamNames : &StructNames;

	if (AddCollidingName(StructNames, TargetNameContainer, NameIdx, CurrentType, false))
		return TargetNameContainer->size() - 1;

	for (UEStruct Current = Struct.GetSuper(); Current; Current = Current.GetSuper())
	{
		NameContainer& SuperNames = NameInfos[Current.GetIndex()];

		if (AddCollidingName(SuperNames, TargetNameContainer, NameIdx, CurrentType, true))
			return TargetNameContainer->size() - 1;
	}

	/* Serach ReservedNames last, just in case there was a property in the struct or parent struct, which also collided with a reserved name already */
	if (AddCollidingName(ReservedNames, TargetNameContainer, NameIdx, CurrentType, false))
		return TargetNameContainer->size() - 1;

	// Create new empty NameInfo
	if (bIsParameter && FuncParamNames)
	{
		FuncParamNames->emplace_back(NameIdx, CurrentType);
		return FuncParamNames->size() - 1;
	}
	else
	{
		StructNames.emplace_back(NameIdx, CurrentType);
		return StructNames.size() - 1;
	}
}

void CollisionManager::AddReservedName(const std::string& Name, bool bIsParameterOrLocalVariable)
{
	NameInfo NewInfo;
	NewInfo.Name = MemberNames.FindOrAdd(Name).first;
	NewInfo.CollisionData = 0x0;
	NewInfo.OwnType = static_cast<uint8>(bIsParameterOrLocalVariable ? ECollisionType::ParameterName : ECollisionType::SuperMemberName);

	ReservedNames.push_back(NewInfo);
}

void CollisionManager::AddStructToNameContainer(UEStruct Struct)
{
	if (UEStruct Super = Struct.GetSuper())
	{
		if (NameInfos.find(Super.GetIndex()) == NameInfos.end())
			AddStructToNameContainer(Super);
	}

	NameContainer& StructNames = NameInfos[Struct.GetIndex()];

	if (!StructNames.empty())
		return;

	auto AddToContainerAndTranslationMap = [&](auto Member, ECollisionType CollisionType, UEFunction Func = nullptr) -> void
	{
		const uint64 Index = AddNameToContainer(StructNames, Struct, MemberNames.FindOrAdd(Member.GetValidName()), CollisionType, Func);

		const auto [It, bInserted] = TranslationMap.emplace(KeyFunctions::GetKeyForCollisionInfo(Struct, Member), Index);
		
		if (!bInserted)
			std::cout << "Error, no insertion took place, key {0x" << std::hex << KeyFunctions::GetKeyForCollisionInfo(Struct, Member) << "} duplicated!" << std::endl;
	};

	for (UEProperty Prop : Struct.GetProperties())
		AddToContainerAndTranslationMap(Prop, ECollisionType::MemberName);

	for (UEFunction Func : Struct.GetFunctions())
	{
		AddToContainerAndTranslationMap(Func, ECollisionType::FunctionName);

		for (UEProperty Prop : Func.GetProperties())
			AddToContainerAndTranslationMap(Prop, ECollisionType::ParameterName, Func);
	}
};

std::string CollisionManager::StringifyName(UEStruct Struct, NameInfo Info)
{
	ECollisionType OwnCollisionType = static_cast<ECollisionType>(Info.OwnType);

	std::string Name = MemberNames.GetStringEntry(Info.Name).GetName();

	//std::cout << "Nm: " << Name << "\n";

	// Order of sub-if-statements matters
	if (OwnCollisionType == ECollisionType::MemberName)
	{
		if (Info.SuperMemberNameCollisionCount > 0x0)
		{
			Name += ("_" + Struct.GetValidName());
		}
		if (Info.MemberNameCollisionCount > 0x0)
		{
			Name += ("_" + std::to_string(Info.MemberNameCollisionCount - 1));
		}
	}
	else if (OwnCollisionType == ECollisionType::FunctionName)
	{
		if (Info.MemberNameCollisionCount > 0x0 || Info.SuperMemberNameCollisionCount > 0x0)
		{
			Name = ("Func_" + Name);
		}
		if (Info.FunctionNameCollisionCount > 0x0)
		{
			Name += ("_" + std::to_string(Info.FunctionNameCollisionCount - 1));
		}
	}
	else if (OwnCollisionType == ECollisionType::ParameterName)
	{
		if (Info.MemberNameCollisionCount > 0x0 || Info.SuperMemberNameCollisionCount > 0x0 || Info.FunctionNameCollisionCount > 0x0 || Info.SuperFuncNameCollisionCount > 0x0)
		{
			Name = ("Param_" + Name);
		}
		if (Info.ParamNameCollisionCount > 0x0)
		{
			Name += ("_" + std::to_string(Info.ParamNameCollisionCount - 1));
		}
	}

	return Name;
}