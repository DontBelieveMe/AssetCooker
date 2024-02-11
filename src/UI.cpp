#include "App.h"
#include "UI.h"


#include "FileSystem.h"
#include "CookingSystem.h"
#include "Debug.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "Ticks.h"

#include "win32/misc.h"
#include "win32/window.h"

extern "C" __declspec(dllimport) HINSTANCE WINAPI ShellExecuteA(HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR lpParameters, LPCSTR lpDirectory, INT nShowCmd);


ImGuiStyle             gStyle                           = {};

bool                   gOpenImGuiDemo                   = false;
bool                   gOpenDebugWindow                 = false;
CookingLogEntryID      gSelectedCookingLogEntry         = {};
bool                   gScrollToSelectedCookingLogEntry = false;

constexpr const char*  cAppLogWindowName                = "App Log";
constexpr const char*  cCommandOutputWindowName         = "Command Output";

int64                  gCurrentTimeInTicks              = 0;

// TODO these colors are terrible
constexpr uint32       cColorTextError                  = IM_COL32(255, 100, 100, 255);
constexpr uint32       cColorTextSuccess                = IM_COL32( 98, 214,  86, 255);
constexpr uint32       cColorFrameBgError               = IM_COL32(150,  60,  60, 255);


StringView gGetAnimatedHourglass()
{
	constexpr StringView hourglass[] = { ICON_FK_HOURGLASS_START, ICON_FK_HOURGLASS_HALF, ICON_FK_HOURGLASS_END };
	return hourglass[(int)(gTicksToSeconds(gCurrentTimeInTicks) * 4.0) % gElemCount(hourglass)];
}


Span<const uint8> gGetEmbeddedFont(StringView inName)
{
	HRSRC   resource             = FindResourceA(nullptr, inName.AsCStr(), "FONTFILE");
	DWORD   resource_data_size   = SizeofResource(nullptr, resource);
	HGLOBAL resource_data_handle = LoadResource(nullptr, resource);
	auto    resource_data        = (const uint8*)LockResource(resource_data_handle);

	return { resource_data, resource_data_size };
}

struct UIScale
{
	static constexpr float cMin = 0.4f;
	static constexpr float cMax = 3.0f;

	float mFromDPI      = 1.0f;
	float mFromSettings = 1.0f;
	bool  mNeedUpdate   = true;

	float GetFinalScale() const { return mFromDPI * mFromSettings; }
};

UIScale gUIScale;


// Set the DPI scale.
void gUISetDPIScale(float inDPIScale)
{
	if (inDPIScale == gUIScale.mFromDPI)
		return;

	gUIScale.mFromDPI = inDPIScale;
	gUIScale.mNeedUpdate = true;
}


// Set the user setting scale.
void gUISetScale(float inScale)
{
	float scale = gClamp(inScale, UIScale::cMin, UIScale::cMax);
	if (scale == gUIScale.mFromSettings)
		return;

	gUIScale.mFromSettings = scale;
	gUIScale.mNeedUpdate   = true;
}


void gUIUpdate()
{
	if (gUIScale.mNeedUpdate)
	{
		gUIScale.mNeedUpdate = false;

		ImGui::GetStyle() = gStyle;
		ImGui::GetStyle().ScaleAllSizes(gUIScale.GetFinalScale());

		auto& io = ImGui::GetIO();

		// Remove all the font data.
		io.Fonts->Clear();
		// Release the DX11 objects (font textures, but also everything else... might not be the most efficient).
		ImGui_ImplDX11_InvalidateDeviceObjects();

		// Fonts are embedded in the exe, they don't need to be released.
		ImFontConfig font_config; 
		font_config.FontDataOwnedByAtlas = false;

		// Reload the fonts at the new scale.
		// The main font.
		{
			auto cousine_ttf = gGetEmbeddedFont("cousine_regular");
			io.Fonts->AddFontFromMemoryTTF((void*)cousine_ttf.data(), (int)cousine_ttf.size(), 14.0f * gUIScale.GetFinalScale(), &font_config);
		}

		// The icons font.
		{
			ImFontConfig icons_config          = font_config;
			icons_config.MergeMode             = true; // Merge inot the default font.
			icons_config.GlyphOffset.y         = 0;

			static const ImWchar icon_ranges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 };

			auto ttf_data = gGetEmbeddedFont("forkawesome");
			io.Fonts->AddFontFromMemoryTTF((void*)ttf_data.data(), (int)ttf_data.size(), 14.0f * gUIScale.GetFinalScale(), &icons_config, icon_ranges);
		}

		// Re-create the DX11 objects.
		ImGui_ImplDX11_CreateDeviceObjects();
	}
}


void gDrawMainMenuBar()
{
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Exit", "Alt + F4"))
				gApp.RequestExit();

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Settings"))
		{
			float ui_scale = gUIScale.mFromSettings;
			if (ImGui::DragFloat("UI Scale", &ui_scale, 0.01f, UIScale::cMin, UIScale::cMax, "%.1f"))
				gUISetScale(ui_scale);

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Debug"))
		{
			if (ImGui::BeginMenu("Log FileSystem Activity"))
			{
				const char* log_levels[] = { "None", "Basic", "Verbose" };
				static_assert(gElemCount(log_levels) == (size_t)LogLevel::Verbose + 1);

				int current_log_level = (int)gApp.mLogFSActivity;

				if (ImGui::ListBox("##Verbosity", &current_log_level, log_levels, gElemCount(log_levels)))
					gApp.mLogFSActivity = (LogLevel)current_log_level;

				ImGui::EndMenu();
			}

			ImGui::MenuItem("ImGui Demo Window", nullptr, &gOpenImGuiDemo);
			ImGui::MenuItem("Debug Window", nullptr, &gOpenDebugWindow);

			ImGui::MenuItem("Make Cooking Slower", nullptr, &gCookingSystem.mSlowMode);

			ImGui::EndMenu();
		}

		ImGui::EndMainMenuBar();
	}
}



void gDrawFileInfoSpan(StringView inListName, Span<const FileID> inFileIDs);
void gDrawCookingCommandSpan(StringView inListName, Span<const CookingCommandID> inCommandIDs);


TempString256 gFormat(const CookingCommand& inCommand)
{
	return { "{}{} {}",
		inCommand.GetRule().mName,
		inCommand.NeedsCleanup() ? " (Cleanup)" : "",
		inCommand.GetMainInput().GetFile() };
}


TempString256 gFormat(const CookingLogEntry& inLogEntry)
{
	const CookingCommand& command = gCookingSystem.GetCommand(inLogEntry.mCommandID);
	const CookingRule&    rule    = gCookingSystem.GetRule(command.mRuleID);
	SystemTime            start_time = inLogEntry.mTimeStart.ToLocalTime();
	return { "[#{} {:02}:{:02}:{:02}] {}{:10} {} - {}",
		rule.mPriority,
		start_time.mHour, start_time.mMinute, start_time.mSecond,
		command.GetRule().mName,
		inLogEntry.mIsCleanup ? " (Cleanup)" : "",
		command.GetMainInput().GetFile().mPath, gToStringView(inLogEntry.mCookingState) };
}


TempString512 gFormat(const FileInfo& inFile)
{
	return { "{}", inFile };
}


void gDrawFileInfo(const FileInfo& inFile)
{
	ImGui::PushID(TempString32("File {}", inFile.mID.AsUInt()).AsCStr());
	defer { ImGui::PopID(); };

	bool clicked = ImGui::Selectable(gFormat(inFile).AsCStr(), false, ImGuiSelectableFlags_DontClosePopups);
	bool open    = ImGui::IsItemHovered() && ImGui::IsMouseClicked(1);

	if (open)
		ImGui::OpenPopup("Popup");

	if (ImGui::IsPopupOpen("Popup") && 
		ImGui::BeginPopupWithTitle("Popup", TempString128("{} {}: ...\\{}", 
			inFile.IsDirectory() ? ICON_FK_FOLDER_OPEN_O : ICON_FK_FILE_O,
			inFile.GetRepo().mName, inFile.GetName()).AsCStr()))
	{
		ImGui::Spacing();
		// TODO auto wrapping is kind of incompatible with window auto resizing, need to provide a wrap position, or maybe make sure it isn't the first item drawn?
		// https://github.com/ocornut/imgui/issues/778#issuecomment-239696811
		//ImGui::PushTextWrapPos(0.0f);
		ImGui::Text(gFormat(inFile));
		//ImGui::PopTextWrapPos();
		ImGui::Spacing();

		// TODO make it clearer when we're looking at a deleted file

		if (!inFile.IsDeleted() && ImGui::ButtonGrad("Show in Explorer"))
		{
			// The more common version, doesn't open a new window if there's already one, but doesn't allow selecting a file. 
			//ShellExecuteA(nullptr, "explore", TempString512("{}{}", inFile.GetRepo().mRootPath, inFile.GetDirectory()).AsCStr(), nullptr, nullptr, SW_SHOWDEFAULT);

			// Always open a new window, but at least selects the file.
			ShellExecuteA(nullptr, nullptr, "explorer", TempString512("/select, {}{}", inFile.GetRepo().mRootPath, inFile.mPath).AsCStr(), nullptr, SW_SHOWDEFAULT);
		}

		ImGui::SeparatorText("Details");

		if (ImGui::BeginTable("File Details", 2))
		{
			ImGui::TableNextRow();
			
			ImGui::TableNextColumn(); ImGui::TextUnformatted("Repo");
			ImGui::TableNextColumn(); ImGui::Text(TempString128("{} ({})", inFile.GetRepo().mName, inFile.GetRepo().mRootPath));

			if (inFile.IsDeleted())
			{
				ImGui::TableNextColumn(); ImGui::TextUnformatted("Deletion Time");
				ImGui::TableNextColumn(); ImGui::Text(TempString64("{}", inFile.mCreationTime));
			}
			else
			{
				ImGui::TableNextColumn(); ImGui::TextUnformatted("RefNumber");
				ImGui::TableNextColumn(); ImGui::Text(TempString64("{}", inFile.mRefNumber));
				
				ImGui::TableNextColumn(); ImGui::TextUnformatted("Creation Time");
				ImGui::TableNextColumn(); ImGui::Text(TempString64("{}", inFile.mCreationTime));

				ImGui::TableNextColumn(); ImGui::TextUnformatted("Last Change Time");
				ImGui::TableNextColumn(); ImGui::Text(TempString64("{}", inFile.mLastChangeTime));
				
				ImGui::TableNextColumn(); ImGui::TextUnformatted("Last Change USN");
				ImGui::TableNextColumn(); ImGui::Text(TempString64("{}", inFile.mLastChangeUSN));
			}

			ImGui::EndTable();
		}

		ImGui::SeparatorText("Related Commands");

		if (!inFile.mInputOf.empty())
			gDrawCookingCommandSpan("Is Input Of", inFile.mInputOf);
		if (!inFile.mOutputOf.empty())
			gDrawCookingCommandSpan("Is Output Of", inFile.mOutputOf);


		ImGui::EndPopup();
	}
}


void gSelectCookingLogEntry(CookingLogEntryID inLogEntryID, bool inScrollLog)
{
	gSelectedCookingLogEntry         = inLogEntryID;
	gScrollToSelectedCookingLogEntry = inScrollLog;
	ImGui::SetWindowFocus(cCommandOutputWindowName);
}


void gDrawCookingCommandPopup(const CookingCommand& inCommand)
{
	if (!ImGui::IsPopupOpen("Popup"))
		return;

	if (!ImGui::BeginPopupWithTitle("Popup", TempString512(ICON_FK_CUTLERY " {}{} ...\\{}",
		inCommand.GetRule().mName,
		inCommand.NeedsCleanup() ? " (Cleanup)" : "",
		inCommand.GetMainInput().GetFile().GetName()).AsStringView()))
		return;

	defer { ImGui::EndPopup(); };

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, gStyle.ItemSpacing);

	if (!inCommand.IsCleanedUp() && ImGui::ButtonGrad("Cook"))
		gCookingSystem.ForceCook(inCommand.mID);

	if (inCommand.mLastCookingLog)
	{
		ImGui::SameLine();
		if (ImGui::ButtonGrad("Select last Log"))
		{
			gSelectCookingLogEntry(inCommand.mLastCookingLog->mID, true);
		}
	}

	ImGui::SeparatorText("Cooking State");

	if (inCommand.IsDirty())
	{
		ImGui::TextUnformatted("Dirty");

		ImGui::Indent();

		if (inCommand.mDirtyState & CookingCommand::AllInputsMissing)
		{
			ImGui::TextUnformatted("All Inputs Missing - Needs cleanup");
		}
		else
		{
			if (inCommand.mDirtyState & CookingCommand::InputMissing)
				ImGui::TextUnformatted("Input Missing");
			if (inCommand.mDirtyState & CookingCommand::InputChanged)
				ImGui::TextUnformatted("Input Changed");
			if (inCommand.mDirtyState & CookingCommand::OutputMissing)
				ImGui::TextUnformatted("Output Missing");
		}

		ImGui::Unindent();
	}
	else
	{
		if (inCommand.IsCleanedUp())
			ImGui::TextUnformatted("Cleaned Up");
		else
			ImGui::TextUnformatted("Up To Date");
	}

	ImGui::SeparatorText("Related Files");

	gDrawFileInfoSpan("Inputs", inCommand.mInputs);
	gDrawFileInfoSpan("Outputs", inCommand.mOutputs);

	ImGui::PopStyleVar();
}


void gDrawCookingCommand(const CookingCommand& inCommand)
{
	ImGui::PushID(TempString32("Command {}", inCommand.mID.mIndex).AsCStr());
	defer { ImGui::PopID(); };
	
	int pop_color = 0;

	if (inCommand.GetCookingState() == CookingState::Error)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, cColorTextError);
		pop_color++;
	}

	bool clicked = ImGui::Selectable(gFormat(inCommand).AsCStr(), false, ImGuiSelectableFlags_DontClosePopups);
	bool open    = ImGui::IsItemHovered() && ImGui::IsMouseClicked(1);

	if (pop_color)
		ImGui::PopStyleColor(pop_color);

	if (open)
		ImGui::OpenPopup("Popup");

	gDrawCookingCommandPopup(inCommand);
}



void gDrawFileInfoSpan(StringView inListName, Span<const FileID> inFileIDs)
{
	constexpr int cMaxItemsForOpenByDefault = 10;
	ImGui::SetNextItemOpen(inListName.size() <= cMaxItemsForOpenByDefault, ImGuiCond_Appearing);

	if (ImGui::TreeNode(inListName.data(), TempString64("{} ({} items)", inListName, inFileIDs.size()).AsCStr()))
	{
		for (FileID file_id : inFileIDs)
		{
			gDrawFileInfo(file_id.GetFile());
		}
		ImGui::TreePop();
	}
};


void gDrawCookingCommandSpan(StringView inListName, Span<const CookingCommandID> inCommandIDs)
{
	constexpr int cMaxItemsForOpenByDefault = 10;
	ImGui::SetNextItemOpen(inListName.size() <= cMaxItemsForOpenByDefault, ImGuiCond_Appearing);

	if (ImGui::TreeNode(inListName.data(), TempString64("{} ({} items)", inListName, inCommandIDs.size()).AsCStr()))
	{
		for (CookingCommandID command_id : inCommandIDs)
		{
			gDrawCookingCommand(gCookingSystem.GetCommand(command_id));
		}
		ImGui::TreePop();
	}
};


void gDrawCookingQueue()
{
	if (!ImGui::Begin("Cooking Queue"))
	{
		ImGui::End();
		return;
	}

	bool paused = gCookingSystem.IsCookingPaused();
	if (ImGui::ButtonGrad(paused ? ICON_FK_PLAY " Start Cooking" : ICON_FK_STOP " Stop Cooking"))
		gCookingSystem.SetCookingPaused(!paused);

	// Lock the dirty command list while we're browsing it.
	std::lock_guard lock(gCookingSystem.mCommandsDirty.mMutex);

	if (ImGui::BeginChild("ScrollingRegion"))
	{

		for (auto& bucket : gCookingSystem.mCommandsDirty.mPrioBuckets)
		{
			if (bucket.mCommands.empty())
				continue;

			ImGui::SeparatorText(TempString64("Priority {} ({} items)", bucket.mPriority, bucket.mCommands.size()));

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

			ImGuiListClipper clipper;
			clipper.Begin((int)bucket.mCommands.size());
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					gDrawCookingCommand(gCookingSystem.GetCommand(bucket.mCommands[i]));
				}
			}
			clipper.End();

			ImGui::PopStyleVar();
		}
	}
	ImGui::EndChild();

	ImGui::End();
}


void gDrawCookingLog()
{
	bool visible = ImGui::Begin("Cooking Log");
	defer { ImGui::End(); };

	if (!visible)
		return;

	if (!ImGui::BeginTable("CookingLog", 4, ImGuiTableFlags_ScrollY))
		return;
	defer { ImGui::EndTable(); };

	ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);

	ImGui::TableNextRow();

	ImGuiListClipper clipper;
	clipper.Begin((int)gCookingSystem.mCookingLog.Size());
	defer { clipper.End(); };
	while (clipper.Step())
	{
		if (clipper.ItemsHeight != -1.f && gScrollToSelectedCookingLogEntry && gSelectedCookingLogEntry.IsValid())
		{
			gScrollToSelectedCookingLogEntry = false;
            ImGui::SetScrollFromPosY(ImGui::GetCursorStartPos().y + (float)gSelectedCookingLogEntry.mIndex * clipper.ItemsHeight);
		}

		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
		{
			const CookingLogEntry& log_entry = gCookingSystem.mCookingLog[i];
			const CookingCommand&  command   = gCookingSystem.GetCommand(log_entry.mCommandID);
			const CookingRule&     rule      = gCookingSystem.GetRule(command.mRuleID);
			bool                   selected  = (gSelectedCookingLogEntry.mIndex == (uint32)i);

			ImGui::PushID(&log_entry);
			defer { ImGui::PopID(); };

			ImGui::TableNextColumn();
			{
				LocalTime start_time = log_entry.mTimeStart.ToLocalTime();
				if (ImGui::Selectable(TempString32("[#{} {:02}:{:02}:{:02}]", rule.mPriority, start_time.mHour, start_time.mMinute, start_time.mSecond).AsCStr(), selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					gSelectCookingLogEntry({ (uint32)i }, false);
				}

				bool open = ImGui::IsItemHovered() && ImGui::IsMouseClicked(1);
				if (open)
					ImGui::OpenPopup("Popup");

			}

			ImGui::TableNextColumn();
			{
				ImGui::TextUnformatted(command.GetRule().mName);
			}

			ImGui::TableNextColumn();
			{
				ImGui::Text(TempString512("{}", command.GetMainInput().GetFile()));
			}

			ImGui::TableNextColumn();
			{
				constexpr StringView cIcons[]
				{
					ICON_FK_QUESTION,
					ICON_FK_HOURGLASS,
					ICON_FK_HOURGLASS,
					ICON_FK_TIMES,
					ICON_FK_CHECK,
				};
				static_assert(gElemCount(cIcons) == (size_t)CookingState::_Count);

				auto cooking_state = log_entry.mCookingState.load();
				auto icon          = cIcons[(int)cooking_state];

				if (cooking_state == CookingState::Cooking || cooking_state == CookingState::Waiting)
					icon = gGetAnimatedHourglass();

				int pop_color = 0;

				if (cooking_state == CookingState::Success)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, cColorTextSuccess);
					pop_color++;
				}

				if (cooking_state == CookingState::Error)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, cColorTextError);
					pop_color++;
				}

				ImGui::Text(TempString32(" {} ", icon));

				if (pop_color)
					ImGui::PopStyleColor(pop_color);

				ImGui::SetItemTooltip(gToStringView(cooking_state).AsCStr());
			}

			gDrawCookingCommandPopup(gCookingSystem.GetCommand(log_entry.mCommandID));
		}
	}

	// Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
	// Using a scrollbar or mouse-wheel will take away from the bottom edge.
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
		ImGui::SetScrollHereY(1.0f);
}


void gDrawSelectedCookingLogEntry()
{
	bool opened = ImGui::Begin(cCommandOutputWindowName);
	defer { ImGui::End(); };

	if (!opened || !gSelectedCookingLogEntry.IsValid())
		return;

	const CookingLogEntry& log_entry = gCookingSystem.GetLogEntry(gSelectedCookingLogEntry);

	ImGui::TextUnformatted(gFormat(log_entry));

	if (ImGui::BeginChild("ScrollingRegion", {}, ImGuiChildFlags_FrameStyle, ImGuiWindowFlags_HorizontalScrollbar))
	{
		// If it's finished cooking, it's safe to read the log output.
		if (log_entry.mCookingState > CookingState::Cooking)
		{
			//ImGui::PushTextWrapPos();
			ImGui::TextUnformatted(log_entry.mOutput);
			//ImGui::PopTextWrapPos();
		}
	}
	ImGui::EndChild();
}


void gDrawCommandSearch()
{
	if (!ImGui::Begin("Command Search"))
	{
		ImGui::End();
		return;
	}

	if (ImGui::BeginChild("ScrollingRegion"))
	{
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

		ImGuiListClipper clipper;
		clipper.Begin((int)gCookingSystem.mCommands.Size());
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
			{
				const CookingCommand& command = gCookingSystem.GetCommand(CookingCommandID{ (uint32)i });
				gDrawCookingCommand(command);
			}
		}

		clipper.End();

		ImGui::PopStyleVar();
	}
	ImGui::EndChild();

	ImGui::End();
}


void gDrawCookingThreads()
{
	if (!ImGui::Begin("Worker Threads", nullptr, ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::End();
		return;
	}

	int thread_count = (int)gCookingSystem.mCookingThreads.size();
	int columns      = sqrt(thread_count); // TODO need a max number of columns instead, they're useless if too short

	if (thread_count && ImGui::BeginTable("Threads", columns, ImGuiTableFlags_SizingStretchSame))
	{
		ImGui::TableNextRow();

		for (auto& thread : gCookingSystem.mCookingThreads)
		{
			ImGui::TableNextColumn();
			ImGui::BeginChild(ImGui::GetID(&thread), ImVec2(0, ImGui::GetFrameHeight()), ImGuiChildFlags_FrameStyle);
			
			CookingLogEntryID entry_id = thread.mCurrentLogEntry.load();
			if (entry_id.IsValid())
			{
				const CookingLogEntry& entry_log = gCookingSystem.GetLogEntry(entry_id);
				const CookingCommand&  command   = gCookingSystem.GetCommand(entry_log.mCommandID);

				ImGui::Text(TempString128("{} {}", command.GetRule().mName, command.GetMainInput().GetFile().mPath));

				if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
					gSelectCookingLogEntry(entry_id, true);
			}
			else
			{
				ImGui::TextUnformatted("Idle");
			}

			ImGui::EndChild();
		}

		ImGui::EndTable();
	}

	ImGui::End();
}


void gDrawDebugWindow()
{
	if (!ImGui::Begin("Debug", &gOpenDebugWindow, ImGuiWindowFlags_NoDocking))
	{
		ImGui::End();
		return;
	}

	if (ImGui::ButtonGrad("Cook 100"))
	{
		for (int i = 0; i<gMin(100, (int)gCookingSystem.mCommands.Size()); ++i)
			gCookingSystem.ForceCook(gCookingSystem.mCommands[i].mID);
	}

	ImGui::SameLine();
	if (ImGui::ButtonGrad("Cook 1000"))
	{
		for (int i = 0; i<gMin(1000, (int)gCookingSystem.mCommands.Size()); ++i)
			gCookingSystem.ForceCook(gCookingSystem.mCommands[i].mID);
	}

	ImGui::Checkbox("Slow mode", &gCookingSystem.mSlowMode);

	if (ImGui::CollapsingHeader(TempString64("Rules ({})##Rules", gCookingSystem.mRules.size()).AsCStr()))
	{
		ImGuiListClipper clipper;
		clipper.Begin((int)gCookingSystem.mRules.size());
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
			{
				const CookingRule& rule = gCookingSystem.mRules[i];
				ImGui::Text(rule.mName);
				// TODO add a gDrawCookingRule
			}
		}
		clipper.End();
	}

	if (ImGui::CollapsingHeader(TempString64("Commands ({})##Commands", gCookingSystem.mCommands.Size()).AsCStr()))
	{
		ImGuiListClipper clipper;
		clipper.Begin((int)gCookingSystem.mCommands.Size());
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				gDrawCookingCommand(gCookingSystem.mCommands[i]);
		}
		clipper.End();
	}

	for (FileRepo& repo : gFileSystem.mRepos)
	{
		ImGui::PushID(&repo);
		if (ImGui::CollapsingHeader(TempString128("{} ({}) - {} Files##Repo", repo.mName, repo.mRootPath, repo.mFiles.Size()).AsCStr()))
		{
			ImGuiListClipper clipper;
			clipper.Begin((int)repo.mFiles.Size());
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
					gDrawFileInfo(repo.mFiles[i]);
			}
			clipper.End();
		}
		ImGui::PopID();
	}

	ImGui::End();
}


void gDrawStatusBar()
{
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
	float            height       = ImGui::GetFrameHeight();

	bool             is_error     = gApp.HasInitError();
	int              pop_color    = 0;

	// Pop any style that was pushed.
	defer
	{
		if (pop_color)
			ImGui::PopStyleColor(pop_color);
	};

	if (is_error)
	{
		ImGui::PushStyleColor(ImGuiCol_MenuBarBg, cColorFrameBgError);
		pop_color++;
	}

	if (!ImGui::BeginViewportSideBar("##MainStatusBar", nullptr, ImGuiDir_Down, height, window_flags))
		return;

	defer { ImGui::End(); };

	if (!ImGui::BeginMenuBar())
		return;
	defer { ImGui::EndMenuBar(); };

	if (gApp.HasInitError())
	{
		ImGui::TextUnformatted(StringView(gApp.mInitError));
		return;
	}

	auto init_state = gFileSystem.GetInitState();
	if (init_state < FileSystem::InitState::Ready)
	{
		switch (init_state)
		{
		default:
		case FileSystem::InitState::NotInitialized:
		{
			ImGui::TextUnformatted("Bonjour.");
			break;
		}
		case FileSystem::InitState::Scanning: 
		{
			ImGui::Text(TempString128("{} Scanning... {:5} files found.", gGetAnimatedHourglass(), gFileSystem.GetFileCount()));
			break;
		}
		case FileSystem::InitState::ReadingUSNJournal: 
		{
			ImGui::TextUnformatted(TempString32("{} Reading USN journal...", gGetAnimatedHourglass()));
			break;
		}
		case FileSystem::InitState::ReadingIndividualUSNs: 
		{
			ImGui::TextUnformatted(TempString128("{} Reading individual USNs... {:5}/{}", gGetAnimatedHourglass(),
				gFileSystem.mInitStats.mIndividualUSNFetched.load(), gFileSystem.mInitStats.mIndividualUSNToFetch
			));
			break;
		}
		}

		return;
	}
 
	double seconds_since_ready = gTicksToSeconds(gGetTickCount() - gFileSystem.mInitStats.mReadyTicks);
	if (seconds_since_ready < 8.0)
	{
		ImGui::Text(TempString128(ICON_FK_THUMBS_O_UP " Init complete in {:.2f} seconds. ",	gTicksToSeconds(gFileSystem.mInitStats.mReadyTicks - gProcessStartTicks)));
	}
	else
	{
		ImGui::TextUnformatted(ICON_FK_CUTLERY" Let's get cooking.");
	}

	// Display some stats on the right side of the status bar.
	TempString128 stats_text("{} Files, {} Repos, {} Commands | UI {} FPS (CPU:{:4.2f}ms GPU:{:4.2f}ms)", 
		gFileSystem.GetFileCount(), gFileSystem.GetRepoCount(), gCookingSystem.GetCommandCount(),
		gUILastFrameStats.mFPS, gUILastFrameStats.mCPUMilliseconds, gUILastFrameStats.mGPUMilliseconds);

	float         stats_text_size = ImGui::CalcTextSize(stats_text).x;
	float         available_size  = ImGui::GetWindowContentRegionMax().x;
	ImGui::SameLine(available_size - stats_text_size);
	ImGui::Text(stats_text);
}


void gDrawMain()
{
	gCurrentTimeInTicks = gGetTickCount() - gProcessStartTicks;

	ImGui::DockSpaceOverViewport(nullptr, ImGuiDockNodeFlags_PassthruCentralNode/* | ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoWindowMenuButton*/);

	gApp.mLog.Draw(cAppLogWindowName);

	gDrawCookingQueue();
	gDrawCookingThreads();
	gDrawCookingLog();
	gDrawCommandSearch();
	gDrawSelectedCookingLogEntry();
	gDrawStatusBar();

	if (gOpenDebugWindow)
		gDrawDebugWindow();

	if (gOpenImGuiDemo)
		ImGui::ShowDemoWindow(&gOpenImGuiDemo);

	// If the App failed to Init, set the focus on the Log because it will contain the errors details.
	// Doesn't work until the windows have been drawn once (not sure why), so do it at the end.
	if (gApp.HasInitError())
	{
		// Do it just once though, otherwise we can't select other windows.
		do_once { ImGui::SetWindowFocus(cAppLogWindowName); };
	}
}