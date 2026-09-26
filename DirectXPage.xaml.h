#pragma once

#include "DirectXPage.g.h"
#include "Cemu_UWP_HostMain.h"
#include "Common/DeviceResources.h"
#include <array>
#include <chrono>
#include <string>

namespace Cemu_UWP_Host
{
	public ref class GraphicPackViewModel sealed
	{
	public:
		property Platform::String^ Identity;
		property Platform::String^ Name;
		property Platform::String^ Category;
		property Platform::String^ Description;
		property bool Enabled;
	};

	public ref class DirectXPage sealed
	{
	public:
		DirectXPage();
		virtual ~DirectXPage();
		void SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void ShutdownRuntime();
		void ResumeRuntime();

	private:
		void ConfigureXboxUiScale();
		void Page_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ args);
		void InitializeEmulator(float width, float height);
		void UpdateEmulatorSurfaceSize(float width, float height);
		void OnRendering(Platform::Object^ sender, Platform::Object^ args);
		void OnGamepadAdded(Platform::Object^ sender, Windows::Gaming::Input::Gamepad^ gamepad);
		void OnGamepadRemoved(Platform::Object^ sender, Windows::Gaming::Input::Gamepad^ gamepad);
		void OnCoreWindowKeyDown(Windows::UI::Core::CoreWindow^ sender,
			Windows::UI::Core::KeyEventArgs^ args);
		void OnBackRequested(Platform::Object^ sender,
			Windows::UI::Core::BackRequestedEventArgs^ args);
		void InstallContent_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ImportKeys_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void DownloadGraphicPacks_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void InstallGraphicPacks_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void GraphicPackGame_SelectionChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ args);
		void GraphicPack_Toggled(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ClearShaderCache_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void RefreshLibrary_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ScanExternalStorage_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void RescanExternalStorage_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ForgetExternalStorage_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void InstalledGames_SelectionChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ args);
		void InstalledGames_ItemClick(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::ItemClickEventArgs^ args);
		void StartGame_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void PerformanceMetricsCheckChanged(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void NavigationButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ToolTabs_SelectionChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ args);
		void SettingsSelectionChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ args);
		void SettingsCheckChanged(Platform::Object^ sender,
			Windows::UI::Xaml::RoutedEventArgs^ args);
		void SettingsValueChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ args);
		void PlaceDimensionsFigure_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void RemoveDimensionsFigure_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void MoveDimensionsFigure_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ClearErrors_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void EmulatorViewport_PointerPressed(Platform::Object^ sender,
			Windows::UI::Xaml::Input::PointerRoutedEventArgs^ args);
		void EmulatorViewport_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ args);
		void GraphicPacksList_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ args);
		void EmulatorSurface_CompositionScaleChanged(
			Windows::UI::Xaml::Controls::SwapChainPanel^ sender,
			Platform::Object^ args);
		void FocusEmulatorInput();
		void SetSystemPointerForUi(bool enabled);
		void SetTabsVisible(bool visible);
		void SetGamePresentation(bool running);
		void SetExternalLoadingVisible(bool visible);
		void AppendError(const std::string& message);
		void OnCemuStateChanged(CemuEmbedState state);
		void OnBrokeredProgress(uint64_t bytesCopied, uint64_t totalBytes, const std::string& relativePath);
		void BeginInstall();
		void BeginExternalLaunch(std::function<bool()> launchOperation);
		void BeginExternalStorageScan(const std::vector<Windows::Storage::StorageFolder^>& storageRoots);
		void RememberExternalStorageFolder(Windows::Storage::StorageFolder^ storageRoot);
		void RestoreExternalStorageFolders(bool showEmptyStatus = false);
		void ForgetExternalStorageFolders();
		void RefreshLibrary(bool scanLocalFolder = false);
		void DeleteInstalledTitle(uint64_t titleId);
		void SetLibraryActionsEnabled(bool enabled);
		void UpdateStartButton();
		void UpdateSelectedShaderCount();
		int FindInstalledTitleIndex(uint64_t titleId) const;
		int FindGamepadSlot(Windows::Gaming::Input::Gamepad^ gamepad) const;
		std::wstring GetGamepadIdentity(Windows::Gaming::Input::Gamepad^ gamepad) const;
		std::wstring GetGamepadDisplayName(Windows::Gaming::Input::Gamepad^ gamepad) const;
		int AssignGamepadSlot(Windows::Gaming::Input::Gamepad^ gamepad);
		void DisconnectGamepadSlot(size_t slot);
		void RefreshGamepads();
		void UpdateGamepadStatus();
		void InitializeGlobalGamepadMappingUi();
		void LoadGlobalGamepadMapping();
		void SaveGlobalGamepadMapping();
		std::array<Windows::UI::Xaml::Controls::ComboBox^, 16> GetGlobalGamepadBindingBoxes() const;
		CemuEmbedGamepadState ApplyGlobalGamepadMapping(const CemuEmbedGamepadState& raw) const;
		void ResetGamepadMapping_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void UpdateGamepadVibration();
		void StopGamepadVibration(Windows::Gaming::Input::Gamepad^ gamepad);
		CemuEmbedGamepadState PublishGamepadStates();
		void UpdateActiveAccount();
		void RefreshDimensionsFigures();
		void RefreshGraphicPackGames();
		void RefreshGraphicPacksForSelectedGame();
		void UpdateGraphicPackGridColumns();
		void LoadSettings();
		void SaveSettings();
		void TryConfigureDefaultGamepad();
		void UpdateVirtualMouse(const CemuEmbedGamepadState& gamepad);
		void SetVirtualMouseEnabled(bool enabled);
		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		std::shared_ptr<Cemu_UWP_HostMain> m_main;
		std::vector<InstalledTitle> m_installedTitles;
		std::vector<InstalledTitle> m_externalTitles;
		std::vector<DimensionsFigure> m_dimensionsFigures;
		std::vector<uint64_t> m_graphicPackGameIds;
		Windows::Foundation::EventRegistrationToken m_renderingToken{};
		Windows::Foundation::EventRegistrationToken m_gamepadAddedToken{};
		Windows::Foundation::EventRegistrationToken m_gamepadRemovedToken{};
		Windows::Foundation::EventRegistrationToken m_coreWindowKeyDownToken{};
		Windows::Foundation::EventRegistrationToken m_coreWindowKeyUpToken{};
		Windows::Foundation::EventRegistrationToken m_backRequestedToken{};
		// Keep WinRT controllers discovered on the XAML apartment. Querying
		// Gamepad::Gamepads on every composition frame re-enters Xbox PnP/user
		// association code and can produce E_INVALIDARG/E_ACCESSDENIED failures.
		std::array<Windows::Gaming::Input::Gamepad^, CEMU_EMBED_MAX_GAMEPADS> m_gamepads{};
		std::array<std::wstring, CEMU_EMBED_MAX_GAMEPADS> m_gamepadIds{};
		std::array<std::wstring, CEMU_EMBED_MAX_GAMEPADS> m_gamepadNames{};
		std::array<uint64_t, CEMU_EMBED_MAX_GAMEPADS> m_gamepadDisconnectedOrder{};
		uint64_t m_gamepadDisconnectSequence = 0;
		Windows::UI::Core::CoreCursor^ m_savedSystemPointerCursor = nullptr;
		bool m_systemPointerHidden = false;
		bool m_cemuReady = false;
		bool m_libraryBusy = false;
		bool m_gamepadProfileReady = false;
		bool m_gameRunning = false;
		bool m_externalLoadingVisible = false;
		bool m_virtualMouseEnabled = false;
		bool m_virtualMouseChordHeld = false;
		bool m_optionsChordHeld = false;
		bool m_virtualMouseLeftDown = false;
		bool m_performanceMetricsVisible = false;
		bool m_loadingSettings = false;
		bool m_loadingGraphicPacks = false;
		bool m_runtimeSuspended = false;
		uint64_t m_selectedTitleId = 0;
		bool m_restoringCommittedSelection = false;
		// Xbox input objects belong to the XAML apartment. Keep one snapshot per
		// player and send it to the DLL only when that player's state changed.
		std::array<CemuEmbedGamepadState, CEMU_EMBED_MAX_GAMEPADS> m_lastPublishedGamepadStates{};
		std::array<bool, CEMU_EMBED_MAX_GAMEPADS> m_hasPublishedGamepadStates{};
		std::array<float, CEMU_EMBED_MAX_GAMEPADS> m_lastAppliedGamepadRumble{};
		std::array<int32_t, 16> m_globalGamepadBindings{
			1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
		double m_virtualMouseX = 0.0;
		double m_virtualMouseY = 0.0;
		std::chrono::steady_clock::time_point m_virtualMouseLastUpdate{};
		unsigned int m_gamepadRetryFrames = 0;
		unsigned int m_controllerPollFrames = 0;
	};
}
