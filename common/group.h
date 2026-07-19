#pragma once

#define LC_MAX_GROUP_NAME 64

enum class lcGroupId : uint32_t;

struct lcGroupHistoryState
{
	lcGroupId Id;
	uint32_t ParentIndex;
	QString Name;

	bool operator==(const lcGroupHistoryState& Other) const
	{
		return Id == Other.Id && ParentIndex == Other.ParentIndex && Name == Other.Name;
	}
};

class lcGroup
{
public:
	lcGroup();

	lcGroup* GetTopGroup()
	{
		return mGroup ? mGroup->GetTopGroup() : this;
	}

	lcGroupId GetId() const
	{
		return mId;
	}

	void FileLoad(lcFile* File);
	void CreateName(const std::vector<std::unique_ptr<lcGroup>>& Groups);

	lcGroupHistoryState GetHistoryState(const lcModel* Model) const;
	void SetHistoryState(const lcGroupHistoryState& State, const lcModel* Model);

	lcGroup* mGroup = nullptr;
	QString mName;

	// Tags the 6 separate per-limb-assembly groups a Posable minifig is split into (see
	// lcModel::ShowMinifigDialog) as belonging to the same minifig instance, without making them
	// an actual parent/child group - that would make GetTopGroup() (and therefore a plain click)
	// select the whole figure on every limb, defeating independent per-limb posing. Instead, the
	// Torso group is the designated "root": every assembly group's mMinifigFamily points at
	// Torso's group (Torso's own mMinifigFamily points at itself), and lc_view.cpp's plain-click
	// handler special-cases "clicked group IS its own mMinifigFamily" to mean "select the whole
	// family" - like grabbing a doll by the torso to move it, while any other limb stays
	// individually clickable for posing. Alt+click also selects the whole family regardless of
	// which piece is clicked (lcModel::SelectMinifigFamilyAction), as a power-user shortcut. Non-
	// minifig groups, and non-Posable minifigs (which use a single ordinary group already), leave
	// this null.
	lcGroup* mMinifigFamily = nullptr;

protected:
	lcGroupId mId = static_cast<lcGroupId>(0);

	static lcGroupId mNextId;
};
