#include "pch.h"
#include "DirectXPage.xaml.h"
#include "Localization.h"

#include <windows.ui.xaml.media.dxinterop.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <iomanip>
#include <sstream>
#include <tuple>

using namespace Cemu_UWP_Host;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::System::Profile;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace
{
const auto VisibleValue = static_cast<Windows::UI::Xaml::Visibility>(0);
const auto CollapsedValue = static_cast<Windows::UI::Xaml::Visibility>(1);
constexpr wchar_t LocalInstallFolderName[] = L"GamesToInstall";
constexpr wchar_t LocalGraphicPackMarkerName[] = L"cemu-graphic-pack-installed.txt";
constexpr wchar_t ExternalFolderAccessMetadataPrefix[] = L"CemuUwpExternalRoot|";
constexpr wchar_t PerformanceMetricsSettingName[] = L"PerformanceMetricsVisible";

struct CompositionSwapChainAttachState
{
	CompositionSwapChainAttachState() : completed(CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS)) {}
	~CompositionSwapChainAttachState() { if (completed) CloseHandle(completed); }
	HANDLE completed{};
	HRESULT result{E_FAIL};
};

int32_t SetCompositionSwapChain(void* userData, void* swapChain)
{
	if (!userData)
		return static_cast<int32_t>(E_INVALIDARG);

	auto panel = reinterpret_cast<SwapChainPanel^>(userData);
	Microsoft::WRL::ComPtr<IDXGISwapChain> retainedSwapChain(
		static_cast<IDXGISwapChain*>(swapChain));
	auto attach = [panel, retainedSwapChain]() -> HRESULT
	{
		Microsoft::WRL::ComPtr<ISwapChainPanelNative> panelNative;
		HRESULT hr = reinterpret_cast<IUnknown*>(panel)->QueryInterface(IID_PPV_ARGS(&panelNative));
		if (FAILED(hr))
			return hr;
		return panelNative->SetSwapChain(retainedSwapChain.Get());
	};

	if (panel->Dispatcher->HasThreadAccess)
		return static_cast<int32_t>(attach());

	auto state = std::make_shared<CompositionSwapChainAttachState>();
	if (!state->completed)
		return static_cast<int32_t>(HRESULT_FROM_WIN32(GetLastError()));

	try
	{
		panel->Dispatcher->RunAsync(CoreDispatcherPriority::High,
			ref new DispatchedHandler([attach, state]()
			{
				state->result = attach();
				SetEvent(state->completed);
			}));
	}
	catch (Platform::Exception^ exception)
	{
		return static_cast<int32_t>(exception->HResult);
	}

	const DWORD waitResult = WaitForSingleObjectEx(state->completed, 10000, FALSE);
	if (waitResult != WAIT_OBJECT_0)
		return static_cast<int32_t>(waitResult == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError()));
	return static_cast<int32_t>(state->result);
}

Platform::String^ WinRtString(const wchar_t* value)
{
	return ref new Platform::String(value);
}

std::string ToUtf8(Platform::String^ value)
{
	if (!value || value->IsEmpty())
		return {};
	const int length = WideCharToMultiByte(CP_UTF8, 0, value->Data(),
		static_cast<int>(value->Length()), nullptr, 0, nullptr, nullptr);
	if (length <= 0)
		return {};
	std::string converted(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value->Data(),
		static_cast<int>(value->Length()), &converted[0], length, nullptr, nullptr);
	return converted;
}

struct LocalGameFile
{
	uint64_t id{};
	std::string name;
	std::string path;
	std::string format;
	std::string brokeredRelativePath;
	StorageFolder^ brokeredTitleFolder{ nullptr };
	uint64_t graphicPackTitleId{};
};

struct LocalInstallScanResult
{
	uint32_t discovered{};
	uint32_t failed{};
	uint32_t markerWarnings{};
	uint32_t graphicPacksDiscovered{};
	uint32_t graphicPacksImported{};
	uint32_t graphicPackFailures{};
	uint32_t graphicPacksAlreadyProcessed{};
	std::vector<LocalGameFile> localGameFiles;
};

struct ExternalStorageScanResult
{
	uint32_t storageCount{};
	uint32_t inaccessibleFolderCount{};
	std::vector<LocalGameFile> gameFiles;
};

bool IsSupportedLocalGameExtension(std::string extension)
{
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	return extension == ".wud" || extension == ".wux" ||
		extension == ".iso" || extension == ".wua" ||
		extension == ".wuhb" || extension == ".rpx" ||
		extension == ".elf";
}

uint64_t StableLocalGameId(std::string path)
{
	std::transform(path.begin(), path.end(), path.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	uint64_t hash = 1469598103934665603ull;
	for (const unsigned char value : path)
	{
		hash ^= value;
		hash *= 1099511628211ull;
	}
	// Real Wii U title IDs occupy the low title namespace. Reserve the high
	// nibble for stable host-only list IDs so selection remains unambiguous.
	return (hash & 0x0FFFFFFFFFFFFFFFull) | 0xF000000000000000ull;
}

StorageFolder^ GetOrCreateLocalInstallFolder()
{
	auto root = create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
		WinRtString(LocalInstallFolderName),
		CreationCollisionOption::OpenIfExists)).get();
	if (!root)
		return nullptr;

	// Keep usage instructions beside the drop folder so they are also visible
	// to users copying content through Xbox Device Portal or an FTP client.
	auto readmeItem = create_task(root->TryGetItemAsync(
		WinRtString(L"README.txt"))).get();
	if (!readmeItem)
	{
		auto readme = create_task(root->CreateFileAsync(
			WinRtString(L"README.txt"),
			CreationCollisionOption::OpenIfExists)).get();
		create_task(FileIO::WriteTextAsync(readme,
			WinRtString(L"Cemu local installation folder\r\n\r\n"
			L"Copy each extracted Wii U base game, update, or DLC into its own subfolder.\r\n"
			L"Every title folder must contain code, content, and meta.\r\n"
			L"Graphic Pack folders are detected by their rules.txt file.\r\n"
			L"In the app, select Scan local folder to detect and list the content.\r\n"
			L"Games remain in this folder and launch directly from it; the scan does not install them.\r\n"))).get();
	}
	return root;
}

bool IsExtractedInstallTitle(StorageFolder^ folder)
{
	if (!folder)
		return false;
	auto code = dynamic_cast<StorageFolder^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"code"))).get());
	auto content = dynamic_cast<StorageFolder^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"content"))).get());
	auto meta = dynamic_cast<StorageFolder^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"meta"))).get());
	if (!code || !content || !meta)
		return false;
	return dynamic_cast<StorageFile^>(
		create_task(code->TryGetItemAsync(WinRtString(L"app.xml"))).get()) != nullptr &&
		dynamic_cast<StorageFile^>(
			create_task(meta->TryGetItemAsync(WinRtString(L"meta.xml"))).get()) != nullptr;
}

bool IsGraphicPackCollection(StorageFolder^ folder)
{
	if (!folder || !folder->Name)
		return false;
	const auto name = folder->Name->Data();
	return _wcsicmp(name, L"Graphic Packs") == 0 ||
		_wcsicmp(name, L"graphicPacks") == 0 ||
		_wcsicmp(name, L"downloadedGraphicPacks") == 0;
}

void FindLocalStorageContent(StorageFolder^ folder,
	std::vector<StorageFolder^>& graphicPacks,
	uint32_t& graphicPacksAlreadyProcessed,
	std::vector<LocalGameFile>& localGameFiles)
{
	if (!folder)
		return;
	if (IsExtractedInstallTitle(folder))
	{
		const auto path = ToUtf8(folder->Path);
		if (!path.empty())
			localGameFiles.push_back({ StableLocalGameId(path), ToUtf8(folder->Name),
				path, "EXTRACTED" });
		// code/content/meta can contain many directories. Once a title root is
		// recognized, do not recurse into its payload.
		return;
	}
	if (dynamic_cast<StorageFile^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"title.tmd"))).get()))
	{
		const auto path = ToUtf8(folder->Path);
		if (!path.empty())
			localGameFiles.push_back({ StableLocalGameId(path), ToUtf8(folder->Name),
				path, "NUS TITLE" });
		return;
	}
	if (IsGraphicPackCollection(folder))
	{
		if (create_task(folder->TryGetItemAsync(
			WinRtString(LocalGraphicPackMarkerName))).get())
			++graphicPacksAlreadyProcessed;
		else
			graphicPacks.emplace_back(folder);
		// Import a community collection in one broker operation instead of one
		// staging pass per rules.txt directory.
		return;
	}
	if (dynamic_cast<StorageFile^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"rules.txt"))).get()))
	{
		if (create_task(folder->TryGetItemAsync(
			WinRtString(LocalGraphicPackMarkerName))).get())
			++graphicPacksAlreadyProcessed;
		else
			graphicPacks.emplace_back(folder);
		// A rules.txt directory is one complete Cemu Graphic Pack. Its shader,
		// patch, and preset subdirectories belong to that pack.
		return;
	}

	const auto files = create_task(folder->GetFilesAsync()).get();
	for (const auto& file : files)
	{
		auto extension = ToUtf8(file->FileType);
		if (!IsSupportedLocalGameExtension(extension))
			continue;
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
		const auto path = ToUtf8(file->Path);
		localGameFiles.push_back({ StableLocalGameId(path), ToUtf8(file->Name),
			path, extension });
	}

	const auto children = create_task(folder->GetFoldersAsync()).get();
	for (const auto& child : children)
		FindLocalStorageContent(child, graphicPacks,
			graphicPacksAlreadyProcessed, localGameFiles);
}

void FindExternalStorageContent(StorageFolder^ folder,
	ExternalStorageScanResult& result)
{
	if (!folder || IsGraphicPackCollection(folder))
		return;
	try
	{
		const auto folderPath = ToUtf8(folder->Path);
		if (IsExtractedInstallTitle(folder))
		{
			if (!folderPath.empty())
			{
				result.gameFiles.push_back({ StableLocalGameId(folderPath),
					ToUtf8(folder->Name), folderPath, "EXTRACTED", {}, folder });
			}
			return;
		}

		if (dynamic_cast<StorageFile^>(
			create_task(folder->TryGetItemAsync(WinRtString(L"title.tmd"))).get()))
		{
			if (!folderPath.empty())
			{
				result.gameFiles.push_back({ StableLocalGameId(folderPath),
					ToUtf8(folder->Name), folderPath, "NUS TITLE", {}, folder });
			}
			return;
		}

		const auto files = create_task(folder->GetFilesAsync()).get();
		for (const auto& file : files)
		{
			auto extension = ToUtf8(file->FileType);
			if (!IsSupportedLocalGameExtension(extension))
				continue;
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
			const auto path = ToUtf8(file->Path);
			if (path.empty())
				continue;
			result.gameFiles.push_back({ StableLocalGameId(path), ToUtf8(file->Name),
				path, extension, ToUtf8(file->Name), folder });
		}

		const auto children = create_task(folder->GetFoldersAsync()).get();
		for (const auto& child : children)
			FindExternalStorageContent(child, result);
	}
	catch (Platform::Exception^)
	{
		++result.inaccessibleFolderCount;
	}
	catch (...)
	{
		++result.inaccessibleFolderCount;
	}
}

void RemoveDuplicateExternalPaths(ExternalStorageScanResult& result)
{
	auto comparePaths = [](const std::string& left, const std::string& right)
	{
		std::string normalizedLeft = left;
		std::string normalizedRight = right;
		std::transform(normalizedLeft.begin(), normalizedLeft.end(), normalizedLeft.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		std::transform(normalizedRight.begin(), normalizedRight.end(), normalizedRight.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		return normalizedLeft < normalizedRight;
	};
	std::sort(result.gameFiles.begin(), result.gameFiles.end(),
		[&comparePaths](const LocalGameFile& left, const LocalGameFile& right)
		{
			return comparePaths(left.path, right.path);
		});
	result.gameFiles.erase(std::unique(result.gameFiles.begin(), result.gameFiles.end(),
		[&comparePaths](const LocalGameFile& left, const LocalGameFile& right)
		{
			return !comparePaths(left.path, right.path) &&
				!comparePaths(right.path, left.path);
		}), result.gameFiles.end());
}

Platform::String^ FromUtf8(const std::string& value)
{
	if (value.empty()) return "";
	const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return "";
	std::wstring converted(length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &converted[0], length);
	return ref new Platform::String(converted.c_str(), static_cast<unsigned int>(converted.size()));
}

bool HasGamepadButton(GamepadButtons buttons, GamepadButtons button)
{
	return (static_cast<unsigned int>(buttons) & static_cast<unsigned int>(button)) != 0;
}

uint32_t NormalizeGamepadButtons(GamepadButtons buttons)
{
	uint32_t result = 0;
	auto set = [&result, buttons](GamepadButtons button, uint32_t bit)
	{
		if (HasGamepadButton(buttons, button)) result |= 1u << bit;
	};
	set(GamepadButtons::A, 0); set(GamepadButtons::B, 1);
	set(GamepadButtons::X, 2); set(GamepadButtons::Y, 3);
	set(GamepadButtons::View, 4); set(GamepadButtons::Menu, 6);
	set(GamepadButtons::LeftThumbstick, 7); set(GamepadButtons::RightThumbstick, 8);
	set(GamepadButtons::LeftShoulder, 9); set(GamepadButtons::RightShoulder, 10);
	set(GamepadButtons::DPadUp, 11); set(GamepadButtons::DPadDown, 12);
	set(GamepadButtons::DPadLeft, 13); set(GamepadButtons::DPadRight, 14);
	return result;
}

double ApplyStickDeadzone(double value)
{
	constexpr double deadzone = 0.18;
	const double magnitude = std::abs(value);
	if (magnitude <= deadzone)
		return 0.0;
	const double normalized = (magnitude - deadzone) / (1.0 - deadzone);
	return std::copysign(normalized, value);
}

bool IsGamepadVirtualKey(VirtualKey key)
{
	switch (key)
	{
	case VirtualKey::GamepadA:
	case VirtualKey::GamepadB:
	case VirtualKey::GamepadX:
	case VirtualKey::GamepadY:
	case VirtualKey::GamepadRightShoulder:
	case VirtualKey::GamepadLeftShoulder:
	case VirtualKey::GamepadLeftTrigger:
	case VirtualKey::GamepadRightTrigger:
	case VirtualKey::GamepadDPadUp:
	case VirtualKey::GamepadDPadDown:
	case VirtualKey::GamepadDPadLeft:
	case VirtualKey::GamepadDPadRight:
	case VirtualKey::GamepadMenu:
	case VirtualKey::GamepadView:
	case VirtualKey::GamepadLeftThumbstickButton:
	case VirtualKey::GamepadRightThumbstickButton:
	case VirtualKey::GamepadLeftThumbstickUp:
	case VirtualKey::GamepadLeftThumbstickDown:
	case VirtualKey::GamepadLeftThumbstickRight:
	case VirtualKey::GamepadLeftThumbstickLeft:
	case VirtualKey::GamepadRightThumbstickUp:
	case VirtualKey::GamepadRightThumbstickDown:
	case VirtualKey::GamepadRightThumbstickRight:
	case VirtualKey::GamepadRightThumbstickLeft:
		return true;
	default:
		return false;
	}
}

bool IsGamepadThumbstickNavigationKey(VirtualKey key)
{
	switch (key)
	{
	case VirtualKey::GamepadLeftThumbstickUp:
	case VirtualKey::GamepadLeftThumbstickDown:
	case VirtualKey::GamepadLeftThumbstickRight:
	case VirtualKey::GamepadLeftThumbstickLeft:
	case VirtualKey::GamepadRightThumbstickUp:
	case VirtualKey::GamepadRightThumbstickDown:
	case VirtualKey::GamepadRightThumbstickRight:
	case VirtualKey::GamepadRightThumbstickLeft:
		return true;
	default:
		return false;
	}
}
}

// This is the desktop interop interface implemented by CoreWindow. Do not use
// the 79B9... IID here: that is the WinRT ICoreWindow interface and has a
// completely different vtable.
struct __declspec(uuid("45D64A29-A63E-4CB6-B498-5781D298CB4F")) ICoreWindowInterop : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND* value) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(boolean value) = 0;
};

namespace
{
constexpr int32_t kDefaultGlobalGamepadBindings[16] =
	{ 1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
}

DirectXPage::DirectXPage()
{
	InitializeComponent();
	Localization::Attach(this);
	InitializeGlobalGamepadMappingUi();
	m_lastAppliedGamepadRumble.fill(-1.0f);
	ConfigureXboxUiScale();
	// Registering WGI events on the XAML thread. The host mirrors a plain
	// controller snapshot into Cemu, so the DLL never has to use a WGI object
	// from SDL's worker apartment on Xbox.
	m_gamepadAddedToken =
		Gamepad::GamepadAdded += ref new EventHandler<Gamepad^>(this, &DirectXPage::OnGamepadAdded);
	m_gamepadRemovedToken =
		Gamepad::GamepadRemoved += ref new EventHandler<Gamepad^>(this, &DirectXPage::OnGamepadRemoved);
	// Xbox also projects controller buttons as CoreWindow keys. Consume that
	// parallel XAML navigation route while a title is running; otherwise B can
	// become a system Back request even though the same button was already sent
	// to Cemu through the apartment-owned WGI snapshot.
	auto coreWindow = Window::Current->CoreWindow;
	m_coreWindowKeyDownToken = coreWindow->KeyDown +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			this, &DirectXPage::OnCoreWindowKeyDown);
	m_coreWindowKeyUpToken = coreWindow->KeyUp +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			this, &DirectXPage::OnCoreWindowKeyDown);
	m_backRequestedToken = SystemNavigationManager::GetForCurrentView()->BackRequested +=
		ref new EventHandler<BackRequestedEventArgs^>(this, &DirectXPage::OnBackRequested);
	// Added is not guaranteed to be replayed for controllers connected before
	// app startup. Enumerate once, then keep stable apartment-owned player slots.
	RefreshGamepads();
	m_renderingToken =
		CompositionTarget::Rendering += ref new EventHandler<Platform::Object^>(this, &DirectXPage::OnRendering);
	UpdateGamepadStatus();
}

void DirectXPage::ConfigureXboxUiScale()
{
	try
	{
		auto versionInfo = AnalyticsInfo::VersionInfo;
		if (!versionInfo || versionInfo->DeviceFamily != "Windows.Xbox")
			return;

		constexpr double uiScale = 1.6;
		constexpr double navigationDesignWidth = 218.0;
		const auto bounds = Window::Current->Bounds;
		const double designWidth = bounds.Width / uiScale;
		const double designHeight = bounds.Height / uiScale;
		const double stageDesignWidth = designWidth - navigationDesignWidth;
		if (stageDesignWidth <= 0.0 || designHeight <= 0.0)
			return;

		// Leave the SwapChainPanel in the native Xbox UWP layout. Only the XAML
		// chrome is laid out at 62.5% size and then rendered at 160%.
		navigationColumn->Width = GridLength(navigationDesignWidth * uiScale);
		navigationRail->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
		navigationRail->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
		navigationRail->Width = navigationDesignWidth;
		navigationRail->Height = designHeight;
		navigationScaleTransform->ScaleX = uiScale;
		navigationScaleTransform->ScaleY = uiScale;

		tabsPanel->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
		tabsPanel->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Top;
		tabsPanel->Width = stageDesignWidth;
		tabsPanel->Height = designHeight;
		tabsScaleTransform->ScaleX = uiScale;
		tabsScaleTransform->ScaleY = uiScale;
		externalLoadingScaleTransform->ScaleX = uiScale;
		externalLoadingScaleTransform->ScaleY = uiScale;

		commandFooter->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Left;
		commandFooter->Width = stageDesignWidth;
		footerScaleTransform->ScaleX = uiScale;
		footerScaleTransform->ScaleY = uiScale;
	}
	catch (Platform::Exception^)
	{
		// Keep the normal desktop-sized layout if Xbox detection or sizing fails.
	}
}

void DirectXPage::Page_SizeChanged(Platform::Object^, SizeChangedEventArgs^)
{
	ConfigureXboxUiScale();
}

void DirectXPage::InitializeEmulator(float width, float height)
{
	if (m_main || width <= 0.0f || height <= 0.0f)
		return;

	// Creating the template swap chain in the page constructor gives it a 1x1
	// back buffer because XAML has not performed layout yet. Hand it to Cemu
	// only after the first real SizeChanged event.
	// Cemu owns rendering once the Direct3D interoperability swap chain is attached. Avoid
	// allocating the template's unused D2D/DWrite/WIC and depth resources.
	m_deviceResources = std::make_shared<DX::DeviceResources>(true);
	m_deviceResources->SetSwapChainPanel(emulatorSurface);
	// SetSwapChainPanel already captures ActualWidth/ActualHeight after the
	// SizeChanged event. Resizing the just-created composition swap chain a
	// second time here can race XAML diagnostics/composition and produces
	// spurious E_INVALIDARG/invalid-call diagnostics.
	const auto outputSize = m_deviceResources->GetOutputSize();

	auto window = Window::Current->CoreWindow;
	Microsoft::WRL::ComPtr<ICoreWindowInterop> windowInterop;
	if (SUCCEEDED(reinterpret_cast<IInspectable*>(window)->QueryInterface(
		__uuidof(ICoreWindowInterop), reinterpret_cast<void**>(windowInterop.GetAddressOf()))))
	{
		HWND hwnd = nullptr;
			if (SUCCEEDED(windowInterop->get_WindowHandle(&hwnd)) && hwnd)
			{
				const auto dpiScale = m_deviceResources->GetDpi() / 96.0;
				const CemuEmbedD3D11Surface d3d11Surface{
					sizeof(CemuEmbedD3D11Surface),
					CEMU_EMBED_D3D11_SURFACE_VERSION,
				m_deviceResources->GetD3DDevice(),
				m_deviceResources->GetD3DDeviceContext(),
					m_deviceResources->GetSwapChain(),
					nullptr,
					reinterpret_cast<IInspectable*>(emulatorSurface),
					&SetCompositionSwapChain,
					reinterpret_cast<IInspectable*>(emulatorSurface)
				};
				m_deviceResources->ReleaseSizeDependentResourcesForExternalRenderer();
				m_main = std::make_shared<Cemu_UWP_HostMain>(
					hwnd, d3d11Surface,
					static_cast<int>(outputSize.Width),
					static_cast<int>(outputSize.Height), dpiScale);
			Platform::WeakReference weakThis(this);
			m_main->SetDiagnosticCallback([weakThis](const std::string& message)
			{
				if (auto page = weakThis.Resolve<DirectXPage>())
					page->AppendError(message);
			});
			m_main->SetStateCallback([weakThis](CemuEmbedState state)
			{
				if (auto page = weakThis.Resolve<DirectXPage>())
					page->OnCemuStateChanged(state);
			});
			m_main->SetProgressCallback([weakThis](uint64_t copied, uint64_t total, const std::string& path)
			{
				if (auto page = weakThis.Resolve<DirectXPage>())
					page->OnBrokeredProgress(copied, total, path);
			});
			if (!m_main->Start())
			{
				AppendError("Failed to initialize Cemu or the selected Direct3D backend.");
				launchStatus->Text = "Failed to initialize the emulator";
				m_main.reset();
			}
		}
	}
}

DirectXPage::~DirectXPage()
{
	CompositionTarget::Rendering -= m_renderingToken;
	Gamepad::GamepadAdded -= m_gamepadAddedToken;
	Gamepad::GamepadRemoved -= m_gamepadRemovedToken;
	Window::Current->CoreWindow->KeyDown -= m_coreWindowKeyDownToken;
	Window::Current->CoreWindow->KeyUp -= m_coreWindowKeyUpToken;
	SystemNavigationManager::GetForCurrentView()->BackRequested -= m_backRequestedToken;
	ShutdownRuntime();
}

void DirectXPage::ShutdownRuntime()
{
	if (m_runtimeSuspended)
		return;
	m_runtimeSuspended = true;
	SetSystemPointerForUi(true);
	SetExternalLoadingVisible(false);
	for (size_t slot = 0; slot < m_gamepads.size(); ++slot)
		DisconnectGamepadSlot(slot);
	if (m_main)
	{
		m_main->SetVirtualMouse(0, 0, false, false);
		CemuEmbedGamepadState disconnected{};
		disconnected.struct_size = sizeof(disconnected);
		disconnected.abi_version = CEMU_EMBED_GAMEPAD_VERSION;
		for (uint32_t playerIndex = 0; playerIndex < CEMU_EMBED_MAX_GAMEPADS; ++playerIndex)
			m_main->SetGamepadState(playerIndex, disconnected);
		m_main->Stop();
	}
	m_main.reset();
	if (m_deviceResources)
		m_deviceResources->DetachSwapChainPanel();
	m_deviceResources.reset();
	m_cemuReady = false;
	m_gameRunning = false;
	m_gamepadProfileReady = false;
	m_externalLoadingVisible = false;
	m_hasPublishedGamepadStates.fill(false);
	m_lastAppliedGamepadRumble.fill(-1.0f);
	installedGamesList->Items->Clear();
	graphicPacksList->Items->Clear();
	graphicPackGameBox->Items->Clear();
	dimensionsFigureBox->Items->Clear();
	std::vector<InstalledTitle>().swap(m_installedTitles);
	std::vector<InstalledTitle>().swap(m_externalTitles);
	std::vector<DimensionsFigure>().swap(m_dimensionsFigures);
	std::vector<uint64_t>().swap(m_graphicPackGameIds);
}

void DirectXPage::ResumeRuntime()
{
	if (!m_runtimeSuspended)
		return;
	m_runtimeSuspended = false;
	RefreshGamepads();
	InitializeEmulator(static_cast<float>(emulatorSurface->ActualWidth),
		static_cast<float>(emulatorSurface->ActualHeight));
	UpdateGamepadStatus();
}

std::wstring DirectXPage::GetGamepadIdentity(Gamepad^ gamepad) const
{
	if (!gamepad)
		return {};
	try
	{
		auto raw = RawGameController::FromGameController(gamepad);
		auto value = raw ? raw->NonRoamableId : nullptr;
		return value ? std::wstring(value->Data(), value->Length()) : std::wstring();
	}
	catch (Platform::Exception^)
	{
		return {};
	}
}

std::wstring DirectXPage::GetGamepadDisplayName(Gamepad^ gamepad) const
{
	if (!gamepad)
		return {};
	try
	{
		auto raw = RawGameController::FromGameController(gamepad);
		auto value = raw ? raw->DisplayName : nullptr;
		return value ? std::wstring(value->Data(), value->Length()) : std::wstring();
	}
	catch (Platform::Exception^)
	{
		return {};
	}
}

void DirectXPage::RefreshGamepads()
{
	for (size_t slot = 0; slot < m_gamepads.size(); ++slot)
		DisconnectGamepadSlot(slot);
	m_hasPublishedGamepadStates.fill(false);
	m_lastAppliedGamepadRumble.fill(-1.0f);
	try
	{
		auto gamepads = Gamepad::Gamepads;
		if (!gamepads)
			return;
		const auto count = (std::min)(static_cast<size_t>(gamepads->Size), m_gamepads.size());
		for (size_t index = 0; index < count; ++index)
			AssignGamepadSlot(gamepads->GetAt(static_cast<unsigned int>(index)));
	}
	catch (Platform::Exception^)
	{
		for (size_t slot = 0; slot < m_gamepads.size(); ++slot)
			DisconnectGamepadSlot(slot);
	}
}

int DirectXPage::FindGamepadSlot(Gamepad^ gamepad) const
{
	if (!gamepad)
		return -1;
	for (size_t index = 0; index < m_gamepads.size(); ++index)
		if (m_gamepads[index] == gamepad)
			return static_cast<int>(index);
	return -1;
}

int DirectXPage::AssignGamepadSlot(Gamepad^ gamepad)
{
	if (!gamepad)
		return -1;
	const int existing = FindGamepadSlot(gamepad);
	if (existing >= 0)
		return existing;

	// A newly attached device must not inherit its previous motor setting.
	StopGamepadVibration(gamepad);
	const auto identity = GetGamepadIdentity(gamepad);
	const auto displayName = GetGamepadDisplayName(gamepad);
	if (!identity.empty())
	{
		for (size_t index = 0; index < m_gamepadIds.size(); ++index)
		{
			if (m_gamepadIds[index] != identity)
				continue;
			// A reconnect may be represented by a new WinRT object. Replace the old
			// handle in-place; a delayed removal event for that stale object then
			// cannot clear the newly reconnected controller.
			if (m_gamepads[index] && m_gamepads[index] != gamepad)
				DisconnectGamepadSlot(index); // Publish the disconnect before replacement.
			m_gamepads[index] = gamepad;
			m_gamepadDisconnectedOrder[index] = 0;
			if (!displayName.empty())
				m_gamepadNames[index] = displayName;
			m_hasPublishedGamepadStates[index] = false;
			m_lastAppliedGamepadRumble[index] = -1.0f;
			return static_cast<int>(index);
		}
	}

	// Prefer a slot that has never belonged to another physical controller.
	for (size_t index = 0; index < m_gamepads.size(); ++index)
	{
		if (m_gamepads[index] != nullptr || !m_gamepadIds[index].empty())
			continue;
		m_gamepads[index] = gamepad;
		m_gamepadIds[index] = identity;
		m_gamepadNames[index] = displayName;
		m_gamepadDisconnectedOrder[index] = 0;
		m_hasPublishedGamepadStates[index] = false;
		m_lastAppliedGamepadRumble[index] = -1.0f;
		return static_cast<int>(index);
	}

	// All player identities have been seen already. Let a genuinely new pad
	// replace the oldest disconnected reservation instead of requiring a restart.
	size_t replacement = m_gamepads.size();
	uint64_t oldestOrder = std::numeric_limits<uint64_t>::max();
	for (size_t index = 0; index < m_gamepads.size(); ++index)
	{
		if (m_gamepads[index] != nullptr)
			continue;
		const auto order = m_gamepadDisconnectedOrder[index];
		if (replacement == m_gamepads.size() || (order != 0 && order < oldestOrder))
		{
			replacement = index;
			oldestOrder = order == 0 ? std::numeric_limits<uint64_t>::max() : order;
		}
	}
	if (replacement != m_gamepads.size())
	{
		m_gamepads[replacement] = gamepad;
		m_gamepadIds[replacement] = identity;
		m_gamepadNames[replacement] = displayName;
		m_gamepadDisconnectedOrder[replacement] = 0;
		m_hasPublishedGamepadStates[replacement] = false;
		m_lastAppliedGamepadRumble[replacement] = -1.0f;
		return static_cast<int>(replacement);
	}
	return -1;
}

void DirectXPage::StopGamepadVibration(Gamepad^ gamepad)
{
	if (!gamepad)
		return;
	try
	{
		GamepadVibration vibration;
		vibration.LeftMotor = 0.0;
		vibration.RightMotor = 0.0;
		vibration.LeftTrigger = 0.0;
		vibration.RightTrigger = 0.0;
		gamepad->Vibration = vibration;
	}
	catch (Platform::Exception^)
	{
	}
}

void DirectXPage::DisconnectGamepadSlot(size_t slot)
{
	if (slot >= m_gamepads.size())
		return;
	if (m_gamepads[slot])
	{
		StopGamepadVibration(m_gamepads[slot]);
		m_gamepads[slot] = nullptr;
		if (!m_gamepadIds[slot].empty())
			m_gamepadDisconnectedOrder[slot] = ++m_gamepadDisconnectSequence;
	}
	// Clear the core's connected state and rumble immediately, not one frame
	// later. This also covers a replacement WinRT object with the same identity.
	if (m_main)
	{
		CemuEmbedGamepadState disconnected{};
		disconnected.struct_size = sizeof(disconnected);
		disconnected.abi_version = CEMU_EMBED_GAMEPAD_VERSION;
		m_main->SetGamepadState(static_cast<uint32_t>(slot), disconnected);
	}
	m_hasPublishedGamepadStates[slot] = false;
	m_lastAppliedGamepadRumble[slot] = -1.0f;
}

void DirectXPage::OnRendering(Platform::Object^, Platform::Object^)
{
	if (m_deviceResources && FAILED(
		m_deviceResources->GetD3DDevice()->GetDeviceRemovedReason()))
	{
		// The core stops its GPU thread as soon as it observes removal. Recreate
		// the host objects here on the XAML thread and republish the complete
		// surface so a subsequent title launch never reuses the removed device.
		m_deviceResources->HandleDeviceLost();
		m_deviceResources->ReleaseSizeDependentResourcesForExternalRenderer();
		if (m_main)
		{
			const CemuEmbedD3D11Surface replacement{
				sizeof(CemuEmbedD3D11Surface), CEMU_EMBED_D3D11_SURFACE_VERSION,
				m_deviceResources->GetD3DDevice(),
				m_deviceResources->GetD3DDeviceContext(),
				m_deviceResources->GetSwapChain(), nullptr,
				reinterpret_cast<IInspectable*>(emulatorSurface),
				&SetCompositionSwapChain,
				reinterpret_cast<IInspectable*>(emulatorSurface) };
			m_main->ReplaceD3D11Surface(replacement);
		}
	}
	// Xbox can recreate its controller-driven system cursor after focus changes
	// or an error dialog. Reassert the hidden cursor while a title owns the
	// presentation, but keep it available for the navigable library.
	if (m_gameRunning)
		SetSystemPointerForUi(false);
	if (m_main) m_main->Pump();
	const auto gamepadState = PublishGamepadStates();
	UpdateGamepadVibration();
	constexpr uint32_t viewButton = 1u << 4;
	constexpr uint32_t menuButton = 1u << 6;
	const bool optionsChord = m_gameRunning &&
		(gamepadState.buttons & viewButton) != 0 &&
		(gamepadState.buttons & menuButton) != 0;
	if (optionsChord && !m_optionsChordHeld)
	{
		const bool showOptions = tabsPanel->Visibility != VisibleValue;
		if (showOptions)
			toolTabs->SelectedIndex = 2;
		SetTabsVisible(showOptions);
	}
	m_optionsChordHeld = optionsChord;
	UpdateVirtualMouse(gamepadState);
	if (++m_controllerPollFrames >= 60)
	{
		m_controllerPollFrames = 0;
		UpdateGamepadStatus();
	}
	// A controller profile changes Cemu's input topology.  On Xbox this must
	// complete before the title starts; changing it while Latte is consuming
	// controller state can terminate the packaged process.
	if (!m_gameRunning && m_cemuReady && !m_gamepadProfileReady &&
		++m_gamepadRetryFrames >= 60)
	{
		m_gamepadRetryFrames = 0;
		TryConfigureDefaultGamepad();
	}
}

void DirectXPage::OnGamepadAdded(Platform::Object^, Gamepad^ gamepad)
{
	// Capture the WGI state while it belongs to the XAML apartment. Cemu uses
	// the mirrored host state instead of opening this controller through SDL.
	Platform::WeakReference weakThis(this);
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([weakThis, gamepad]()
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			// WGI raises device events on a system callback thread. Keep all
			// page state on the XAML dispatcher; racing OnRendering here can make
			// the Xbox terminate the packaged game without a managed exception.
			const int slot = page->AssignGamepadSlot(gamepad);
			if (slot < 0) return;
			if (!page->m_gamepadProfileReady)
				page->m_gamepadRetryFrames = 59;
			page->PublishGamepadStates();
			page->UpdateGamepadStatus();
		})));
}

void DirectXPage::OnGamepadRemoved(Platform::Object^, Gamepad^ gamepad)
{
	// Publish a disconnected host state before updating the UI.
	Platform::WeakReference weakThis(this);
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([weakThis, gamepad]()
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			const int slot = page->FindGamepadSlot(gamepad);
			if (slot >= 0)
				page->DisconnectGamepadSlot(static_cast<size_t>(slot));
			page->PublishGamepadStates();
			if (slot == 0 && page->m_virtualMouseEnabled)
				page->SetVirtualMouseEnabled(false);
			else
				page->UpdateGamepadStatus();
		})));
}

void DirectXPage::OnCoreWindowKeyDown(CoreWindow^, KeyEventArgs^ args)
{
	if (!args || !IsGamepadVirtualKey(args->VirtualKey))
		return;
	// While the options panel is open, projected D-pad/A keys belong to XAML and
	// the WGI snapshot sent to Cemu is neutral. With the panel hidden, every
	// gamepad key belongs exclusively to the emulated Wii U controller.
	const bool optionsVisible = tabsPanel->Visibility == VisibleValue;
	if ((m_gameRunning && !optionsVisible) || IsGamepadThumbstickNavigationKey(args->VirtualKey))
		args->Handled = true;
}

void DirectXPage::OnBackRequested(Platform::Object^, BackRequestedEventArgs^ args)
{
	if (m_gameRunning && args)
	{
		args->Handled = true;
		OutputDebugStringW(L"[Cemu/UWP host] Xbox Back navigation suppressed while a title is running.\n");
	}
}

void DirectXPage::InstallContent_Click(Platform::Object^, RoutedEventArgs^)
{
	BeginInstall();
}

void DirectXPage::ImportKeys_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto picker = ref new FileOpenPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(".txt");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFileAsync()).then([weakThis](StorageFile^ file)
	{
		if (!file) return;
		create_task(FileIO::ReadBufferAsync(file)).then([weakThis](task<IBuffer^> readTask)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			IBuffer^ buffer = nullptr;
			try
			{
				buffer = readTask.get();
			}
			catch (Platform::Exception^ exception)
			{
				page->launchStatus->Text = "keys.txt could not be read";
				page->AppendError("keys.txt read failed: " +
					std::to_string(exception->HResult));
				return;
			}
			if (!buffer || buffer->Length == 0) return;
			auto bytes = ref new Platform::Array<uint8_t>(buffer->Length);
			auto reader = DataReader::FromBuffer(buffer);
			reader->ReadBytes(bytes);
			std::vector<uint8_t> data(bytes->Data, bytes->Data + bytes->Length);
			uint32_t validKeys{};
			if (!page->m_main || !page->m_main->ImportKeys(data, &validKeys))
			{
				page->launchStatus->Text = "keys.txt was not imported";
				page->AppendError("The selected keys.txt contains no valid 128-bit keys or could not be saved.");
				return;
			}
			page->launchStatus->Text = FromUtf8(
				"keys.txt imported: " + std::to_string(validKeys) + " valid key(s)");
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::DownloadGraphicPacks_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	m_libraryBusy = true;
	SetLibraryActionsEnabled(false);
	startButton->IsEnabled = false;
	launchStatus->Text = "Downloading Graphic Packs...";
	const auto main = m_main;
	Platform::WeakReference weakThis(this);
	create_task([main]()
	{
		uint32_t downloaded{};
		bool alreadyCurrent{};
		if (!main || !main->DownloadGraphicPacks(&downloaded, &alreadyCurrent))
			return std::tuple<bool, bool, uint32_t>{ false, false, 0 };
		return std::tuple<bool, bool, uint32_t>{true, alreadyCurrent, downloaded};
	}).then([weakThis](std::tuple<bool, bool, uint32_t> result)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page) return;
		page->m_libraryBusy = false;
		if (!std::get<0>(result))
		{
			page->SetLibraryActionsEnabled(true);
			page->launchStatus->Text = "Failed to download Graphic Packs; see Help and errors";
			page->SetTabsVisible(true);
			page->toolTabs->SelectedIndex = 5;
			page->UpdateStartButton();
			return;
		}
		std::ostringstream status;
		if (std::get<1>(result))
			status << "Graphic Packs are already up to date";
		else
			status << std::get<2>(result) << " Graphic Pack(s) downloaded";
		page->launchStatus->Text = FromUtf8(status.str());
		page->RefreshLibrary();
	}, task_continuation_context::use_current());
}

WriteableBitmap^ DecodeGameIcon(const std::vector<uint8_t>& data)
{
	// Wii U iconTex.tga uses uncompressed 24-bit or 32-bit true-color pixels.
	if (data.size() < 18 || data[1] != 0 || data[2] != 2)
		return nullptr;
	const uint32_t width = static_cast<uint32_t>(data[12]) |
		(static_cast<uint32_t>(data[13]) << 8);
	const uint32_t height = static_cast<uint32_t>(data[14]) |
		(static_cast<uint32_t>(data[15]) << 8);
	const uint32_t bytesPerPixel = data[16] / 8;
	const size_t offset = 18u + data[0];
	if (!width || !height || width > 1024 || height > 1024 ||
		(bytesPerPixel != 3 && bytesPerPixel != 4) ||
		offset + static_cast<size_t>(width) * height * bytesPerPixel > data.size())
		return nullptr;

	auto bitmap = ref new WriteableBitmap(static_cast<int>(width), static_cast<int>(height));
	Microsoft::WRL::ComPtr<IBufferByteAccess> bufferAccess;
	if (FAILED(reinterpret_cast<IInspectable*>(bitmap->PixelBuffer)->QueryInterface(
		IID_PPV_ARGS(bufferAccess.GetAddressOf()))))
		return nullptr;
	byte* destination{};
	if (FAILED(bufferAccess->Buffer(&destination)) || !destination)
		return nullptr;
	const bool topOrigin = (data[17] & 0x20) != 0;
	for (uint32_t y = 0; y < height; ++y)
	{
		const uint32_t sourceY = topOrigin ? y : height - 1 - y;
		const auto* source = data.data() + offset +
			static_cast<size_t>(sourceY) * width * bytesPerPixel;
		auto* target = destination + static_cast<size_t>(y) * width * 4;
		for (uint32_t x = 0; x < width; ++x)
		{
			target[x * 4 + 0] = source[x * bytesPerPixel + 0];
			target[x * 4 + 1] = source[x * bytesPerPixel + 1];
			target[x * 4 + 2] = source[x * bytesPerPixel + 2];
			target[x * 4 + 3] = bytesPerPixel == 4 ? source[x * 4 + 3] : 255;
		}
	}
	bitmap->Invalidate();
	return bitmap;
}

bool IsCemuExternalFolderAccessEntry(const AccessListEntry& entry)
{
	if (!entry.Metadata || !entry.Metadata->Data())
		return false;
	const auto metadata = entry.Metadata->Data();
	const auto prefixLength = wcslen(ExternalFolderAccessMetadataPrefix);
	return wcsncmp(metadata, ExternalFolderAccessMetadataPrefix, prefixLength) == 0;
}

std::vector<Platform::String^> GetPersistedExternalFolderTokens()
{
	std::vector<Platform::String^> tokens;
	auto accessList = StorageApplicationPermissions::FutureAccessList;
	for (const auto& entry : accessList->Entries)
	{
		if (IsCemuExternalFolderAccessEntry(entry))
			tokens.emplace_back(entry.Token);
	}
	return tokens;
}

void DirectXPage::InstallGraphicPacks_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(WinRtString(L"*"));
	picker->CommitButtonText = WinRtString(L"Install graphic packs");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFolderAsync()).then([weakThis](StorageFolder^ folder)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !folder || !page->m_main)
			return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->graphicPacksStatus->Text = "Installing selected graphic packs...";
		const auto main = page->m_main;
		create_task([main, folder]()
		{
			uint32_t imported{};
			return std::make_pair(main->InstallGraphicPacks(folder, &imported), imported);
		}).then([weakThis](std::pair<bool, uint32_t> result)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_libraryBusy = false;
			page->SetLibraryActionsEnabled(true);
			if (!result.first)
			{
				page->graphicPacksStatus->Text = "Graphic pack installation failed.";
				page->AppendError("The selected folder does not contain valid Cemu graphic packs or could not be installed.");
				return;
			}
			page->graphicPacksStatus->Text = FromUtf8(
				std::to_string(result.second) + " graphic pack(s) installed. Configure them individually in this tab.");
			page->RefreshLibrary();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::GraphicPackGame_SelectionChanged(Platform::Object^,
	SelectionChangedEventArgs^)
{
	if (!m_loadingGraphicPacks)
		RefreshGraphicPacksForSelectedGame();
}

void DirectXPage::GraphicPack_Toggled(Platform::Object^ sender, RoutedEventArgs^)
{
	if (m_loadingGraphicPacks || !m_main || m_gameRunning)
		return;
	auto toggle = dynamic_cast<ToggleSwitch^>(sender);
	if (!toggle || !toggle->Tag || graphicPackGameBox->SelectedIndex < 0 ||
		static_cast<size_t>(graphicPackGameBox->SelectedIndex) >= m_graphicPackGameIds.size())
		return;
	auto identity = dynamic_cast<Platform::String^>(toggle->Tag);
	if (!identity || identity->IsEmpty())
		return;
	const uint64_t titleId = m_graphicPackGameIds[graphicPackGameBox->SelectedIndex];
	if (!m_main->SetGraphicPackEnabled(titleId, ToUtf8(identity), toggle->IsOn))
	{
		graphicPacksStatus->Text = "Could not save the selected graphic pack state.";
		m_loadingGraphicPacks = true;
		toggle->IsOn = !toggle->IsOn;
		m_loadingGraphicPacks = false;
		return;
	}
	graphicPacksStatus->Text = toggle->IsOn
		? "Graphic pack enabled for the selected game."
		: "Graphic pack disabled for the selected game.";
}

void DirectXPage::RefreshGraphicPackGames()
{
	uint64_t previousTitleId{};
	if (graphicPackGameBox->SelectedIndex >= 0 &&
		static_cast<size_t>(graphicPackGameBox->SelectedIndex) < m_graphicPackGameIds.size())
		previousTitleId = m_graphicPackGameIds[graphicPackGameBox->SelectedIndex];
	m_loadingGraphicPacks = true;
	graphicPackGameBox->Items->Clear();
	m_graphicPackGameIds.clear();
	int restoredIndex = -1;
	for (const auto& title : m_installedTitles)
	{
		const bool hostOnlyId =
			(title.titleId & 0xF000000000000000ull) == 0xF000000000000000ull;
		const uint64_t graphicPackTitleId = title.graphicPackTitleId
			? title.graphicPackTitleId : (hostOnlyId ? 0 : title.titleId);
		if (!graphicPackTitleId)
			continue;
		if (graphicPackTitleId == previousTitleId && restoredIndex < 0)
			restoredIndex = static_cast<int>(m_graphicPackGameIds.size());
		m_graphicPackGameIds.emplace_back(graphicPackTitleId);
		std::string label = title.name;
		if (!title.localGamePath.empty())
			label += title.isExternalStorage
				? "  [External storage]" : "  [GamesToInstall]";
		graphicPackGameBox->Items->Append(FromUtf8(label));
	}
	graphicPackGameBox->IsEnabled = m_cemuReady && !m_graphicPackGameIds.empty();
	graphicPackGameBox->SelectedIndex = restoredIndex >= 0 ? restoredIndex :
		(m_graphicPackGameIds.empty() ? -1 : 0);
	m_loadingGraphicPacks = false;
	RefreshGraphicPacksForSelectedGame();
}

void DirectXPage::RefreshGraphicPacksForSelectedGame()
{
	m_loadingGraphicPacks = true;
	graphicPacksList->Items->Clear();
	const int index = graphicPackGameBox->SelectedIndex;
	if (!m_main || index < 0 || static_cast<size_t>(index) >= m_graphicPackGameIds.size())
	{
		graphicPacksStatus->Text = "Choose a game to manage its installed graphic packs.";
		m_loadingGraphicPacks = false;
		return;
	}
	const auto packs = m_main->GetGraphicPacksForTitle(m_graphicPackGameIds[index]);
	for (const auto& pack : packs)
	{
		auto item = ref new GraphicPackViewModel();
		item->Identity = FromUtf8(pack.identity);
		item->Name = FromUtf8(pack.name);
		item->Category = FromUtf8(pack.category.empty() ? "General" : pack.category);
		item->Description = FromUtf8(pack.description);
		item->Enabled = pack.enabled;
		graphicPacksList->Items->Append(item);
	}
	graphicPacksStatus->Text = packs.empty()
		? "No installed graphic packs are compatible with this game."
		: FromUtf8(std::to_string(packs.size()) + " compatible graphic pack(s). Changes are saved individually.");
	m_loadingGraphicPacks = false;
	UpdateGraphicPackGridColumns();
}

void DirectXPage::GraphicPacksList_SizeChanged(Platform::Object^,
	SizeChangedEventArgs^)
{
	UpdateGraphicPackGridColumns();
}

void DirectXPage::UpdateGraphicPackGridColumns()
{
	if (!graphicPacksList || graphicPacksList->ActualWidth <= 0)
		return;
	auto panel = dynamic_cast<ItemsWrapGrid^>(graphicPacksList->ItemsPanelRoot);
	if (!panel)
		return;
	// Reserve space for the vertical scrollbar and split the remaining width
	// into four identical slots. Item margins are contained inside each slot.
	constexpr double scrollbarAndEdgeAllowance = 20.0;
	const double usableWidth = (std::max)(0.0,
		graphicPacksList->ActualWidth - scrollbarAndEdgeAllowance);
	panel->ItemWidth = (std::max)(180.0, std::floor(usableWidth / 4.0));
	panel->ItemHeight = 174.0;
}

void DirectXPage::ClearShaderCache_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto dialog = ref new ContentDialog();
	dialog->Title = Localization::GetLiteral(L"Clear shader cache?");
	dialog->Content = Localization::GetLiteral(
		L"Cached shaders for every game will be removed. Games, saves, settings, and Graphic Packs are kept. The next launch rebuilds only the shaders it needs.");
	dialog->PrimaryButtonText = Localization::GetLiteral(L"Clear cache");
	dialog->CloseButtonText = Localization::GetLiteral(L"Cancel");
	Platform::WeakReference weakThis(this);
	create_task(dialog->ShowAsync()).then([weakThis](ContentDialogResult result)
	{
		if (result != ContentDialogResult::Primary)
			return;
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !page->m_main || page->m_libraryBusy || page->m_gameRunning)
			return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->startButton->IsEnabled = false;
		page->launchStatus->Text = "Clearing shader cache...";
		const auto main = page->m_main;
		create_task([main]()
		{
			uint32_t removed{};
			const bool succeeded = main && main->ClearShaderCaches(&removed);
			return std::make_pair(succeeded, removed);
		}).then([weakThis](std::pair<bool, uint32_t> result)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_libraryBusy = false;
			page->SetLibraryActionsEnabled(true);
			if (!result.first)
			{
				page->launchStatus->Text = "Could not clear the shader cache; see Help and errors";
				page->SetTabsVisible(true);
				page->toolTabs->SelectedIndex = 5;
			}
			else
			{
				page->launchStatus->Text = FromUtf8(std::to_string(result.second) +
					" shader-cache entr" + (result.second == 1 ? "y" : "ies") + " cleared");
			}
			page->UpdateStartButton();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::RefreshLibrary_Click(Platform::Object^, RoutedEventArgs^)
{
	RefreshLibrary(true);
}

void DirectXPage::ScanExternalStorage_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append("*");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFolderAsync()).then([weakThis](StorageFolder^ storageRoot)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (page && storageRoot)
		{
			page->RememberExternalStorageFolder(storageRoot);
			page->RestoreExternalStorageFolders();
		}
	}, task_continuation_context::use_current());
}

void DirectXPage::RescanExternalStorage_Click(Platform::Object^, RoutedEventArgs^)
{
	RestoreExternalStorageFolders(true);
}

void DirectXPage::ForgetExternalStorage_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	ForgetExternalStorageFolders();
	m_externalTitles.clear();
	m_selectedTitleId = 0;
	launchStatus->Text = "External folders removed";
	RefreshLibrary();
}

void DirectXPage::RememberExternalStorageFolder(StorageFolder^ storageRoot)
{
	if (!storageRoot)
		return;
	auto accessList = StorageApplicationPermissions::FutureAccessList;
	std::wstring metadata = ExternalFolderAccessMetadataPrefix;
	if (storageRoot->Path)
		metadata += storageRoot->Path->Data();
	else if (storageRoot->Name)
		metadata += storageRoot->Name->Data();
	for (const auto& entry : accessList->Entries)
	{
		if (entry.Metadata && _wcsicmp(entry.Metadata->Data(), metadata.c_str()) == 0)
			return;
	}
	accessList->Add(storageRoot, ref new Platform::String(metadata.c_str()));
}

void DirectXPage::ForgetExternalStorageFolders()
{
	auto accessList = StorageApplicationPermissions::FutureAccessList;
	for (const auto& token : GetPersistedExternalFolderTokens())
	{
		try
		{
			accessList->Remove(token);
		}
		catch (Platform::Exception^)
		{
			// A stale token is already unavailable and does not need user action.
		}
	}
}

void DirectXPage::RestoreExternalStorageFolders(bool showEmptyStatus)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	const auto tokens = GetPersistedExternalFolderTokens();
	if (tokens.empty())
	{
		m_externalTitles.clear();
		if (showEmptyStatus)
			launchStatus->Text = "No external folders saved";
		RefreshLibrary();
		return;
	}
	launchStatus->Text = "Restoring external game folders...";
	Platform::WeakReference weakThis(this);
	create_task([tokens]()
	{
		std::vector<StorageFolder^> roots;
		auto accessList = StorageApplicationPermissions::FutureAccessList;
		for (const auto& token : tokens)
		{
			try
			{
				auto root = create_task(accessList->GetFolderAsync(token)).get();
				if (root)
					roots.emplace_back(root);
			}
			catch (Platform::Exception^)
			{
				// Keep the token so a temporarily disconnected drive can return later.
			}
		}
		return roots;
	}).then([weakThis](std::vector<StorageFolder^> restored)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page)
			return;
		if (restored.empty())
		{
			page->m_externalTitles.clear();
			page->launchStatus->Text = "Saved external folders are disconnected or unavailable";
			page->RefreshLibrary();
			return;
		}
		page->BeginExternalStorageScan(restored);
	}, task_continuation_context::use_current());
}

void DirectXPage::BeginExternalStorageScan(const std::vector<StorageFolder^>& storageRoots)
{
	if (storageRoots.empty() || !m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	m_libraryBusy = true;
	SetLibraryActionsEnabled(false);
	startButton->IsEnabled = false;
	launchStatus->Text = "Scanning saved external storage...";
	Platform::WeakReference weakThis(this);
	const auto main = m_main;
	create_task([storageRoots, main]()
	{
		ExternalStorageScanResult scanResult{};
		for (const auto& storageRoot : storageRoots)
		{
			try
			{
				++scanResult.storageCount;
				FindExternalStorageContent(storageRoot, scanResult);
			}
			catch (Platform::Exception^)
			{
				++scanResult.inaccessibleFolderCount;
			}
			catch (...)
			{
				++scanResult.inaccessibleFolderCount;
			}
		}
		RemoveDuplicateExternalPaths(scanResult);
		if (main)
		{
			for (auto& gameFile : scanResult.gameFiles)
			{
				uint64_t titleId{};
				if (main->IdentifyBrokeredGame(gameFile.brokeredTitleFolder,
					gameFile.brokeredRelativePath, &titleId))
					gameFile.graphicPackTitleId = titleId;
			}
		}
		return scanResult;
	}).then([weakThis](ExternalStorageScanResult scanResult)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page) return;
		page->m_externalTitles.clear();
		page->m_externalTitles.reserve(scanResult.gameFiles.size());
		for (const auto& gameFile : scanResult.gameFiles)
		{
			InstalledTitle externalTitle{};
			externalTitle.titleId = gameFile.id;
			externalTitle.name = gameFile.name;
			externalTitle.regionName = "External storage";
			externalTitle.localGamePath = gameFile.path;
			externalTitle.localGameFormat = gameFile.format;
			externalTitle.brokeredRelativePath = gameFile.brokeredRelativePath;
			externalTitle.brokeredTitleFolder = gameFile.brokeredTitleFolder;
			externalTitle.isExternalStorage = true;
			externalTitle.graphicPackTitleId = gameFile.graphicPackTitleId;
			page->m_externalTitles.emplace_back(std::move(externalTitle));
		}
		page->m_selectedTitleId = 0;
		if (scanResult.inaccessibleFolderCount)
		{
			page->AppendError("Some folders in the selected external storage could not be accessed and were skipped.");
		}
		page->m_libraryBusy = false;
		page->RefreshLibrary();
	}, task_continuation_context::use_current());
}

void DirectXPage::DeleteInstalledTitle(uint64_t titleId)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	const int titleIndex = FindInstalledTitleIndex(titleId);
	if (titleIndex < 0 || !m_installedTitles[titleIndex].localGamePath.empty())
		return;
	const std::string titleName = m_installedTitles[titleIndex].name;
	auto dialog = ref new ContentDialog();
	dialog->Title = Localization::GetLiteral(L"Delete installed game?");
	dialog->Content = Localization::Format(
		L"Delete \"{0}\" together with its installed update and DLC? Save data will be kept.",
		{ FromUtf8(titleName) });
	dialog->PrimaryButtonText = Localization::GetLiteral(L"Delete game");
	dialog->CloseButtonText = Localization::GetLiteral(L"Cancel");
	Platform::WeakReference weakThis(this);
	create_task(dialog->ShowAsync()).then([weakThis, titleId](ContentDialogResult result)
	{
		if (result != ContentDialogResult::Primary)
			return;
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !page->m_main || page->m_libraryBusy || page->m_gameRunning ||
			page->FindInstalledTitleIndex(titleId) < 0)
			return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->startButton->IsEnabled = false;
		page->launchStatus->Text = "Deleting installed game...";
		const auto main = page->m_main;
		create_task([main, titleId]()
		{
			uint32_t removed{};
			const bool succeeded = main && main->DeleteInstalledTitle(titleId, &removed);
			return std::make_pair(succeeded, removed);
		}).then([weakThis](std::pair<bool, uint32_t> result)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_libraryBusy = false;
			if (!result.first)
			{
				page->SetLibraryActionsEnabled(true);
				page->launchStatus->Text = "Could not delete the installed game; see Help and errors";
				page->SetTabsVisible(true);
				page->toolTabs->SelectedIndex = 5;
				page->UpdateStartButton();
				return;
			}
			page->m_selectedTitleId = 0;
			page->launchStatus->Text = FromUtf8(std::to_string(result.second) +
				" installed content folder(s) deleted; saves were kept");
			page->RefreshLibrary();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::InstalledGames_SelectionChanged(Platform::Object^,
	SelectionChangedEventArgs^)
{
	const int selectedIndex = installedGamesList->SelectedIndex;
	const int committedIndex = FindInstalledTitleIndex(m_selectedTitleId);
	if (m_libraryBusy)
	{
		UpdateStartButton();
		return;
	}

	// SelectionChanged is also raised while Xbox moves focus with the D-pad.
	// That is navigation, not confirmation. Only ItemClick (Gamepad A or a
	// pointer click) is allowed to replace m_selectedTitleId.
	if (committedIndex >= 0 && selectedIndex != committedIndex)
	{
		// Xbox's controller-driven pointer can move ListView focus and cause a
		// transient selection change. Restore the title confirmed with A in the
		// same event so the cursor never becomes the authoritative selection.
		if (!m_restoringCommittedSelection)
		{
			m_restoringCommittedSelection = true;
			installedGamesList->SelectedIndex = committedIndex;
			m_restoringCommittedSelection = false;
		}
		launchStatus->Text = "Ready to start";
		UpdateStartButton();
		return;
	}
	else if (committedIndex >= 0)
	{
		launchStatus->Text = "Ready to start";
	}
	UpdateStartButton();
}

void DirectXPage::InstalledGames_ItemClick(Platform::Object^,
	ItemClickEventArgs^ args)
{
	if (!args || !args->ClickedItem || m_libraryBusy)
		return;

	// Xbox pointer/controller activation is not guaranteed to leave the
	// ListViewItem focused after the routed PointerPressed reaches the emulator
	// viewport. Commit the clicked item explicitly instead of depending on the
	// transient focus visual.
	for (unsigned int index = 0; index < installedGamesList->Items->Size; ++index)
	{
		auto item = safe_cast<ListViewItem^>(installedGamesList->Items->GetAt(index));
		if (item != args->ClickedItem && item->Content != args->ClickedItem)
			continue;
		if (index < m_installedTitles.size())
			m_selectedTitleId = m_installedTitles[index].titleId;
		// Commit before assigning SelectedIndex. SelectionChanged can run
		// synchronously and must already see the new title as authoritative.
		installedGamesList->SelectedIndex = static_cast<int>(index);
		launchStatus->Text = "Ready to start";
		UpdateStartButton();
		break;
	}
}

void DirectXPage::StartGame_Click(Platform::Object^, RoutedEventArgs^)
{
	const int selectedIndex = FindInstalledTitleIndex(m_selectedTitleId);
	if (!m_main || !m_main->IsReady() || selectedIndex < 0)
		return;
	const auto& selectedTitle = m_installedTitles[selectedIndex];
	if (!selectedTitle.localGamePath.empty())
	{
		const auto main = m_main;
		if (selectedTitle.isExternalStorage && selectedTitle.brokeredTitleFolder)
		{
			const auto selectedFolder = selectedTitle.brokeredTitleFolder;
			const auto selectedRelativePath = selectedTitle.brokeredRelativePath;
			std::vector<StorageFolder^> supplementalFolders;
			for (const auto& title : m_installedTitles)
			{
				if (title.isExternalStorage && title.brokeredTitleFolder &&
					(title.localGameFormat == "EXTRACTED" || title.localGameFormat == "NUS TITLE"))
					supplementalFolders.emplace_back(title.brokeredTitleFolder);
			}
			BeginExternalLaunch([main, selectedFolder, selectedRelativePath, supplementalFolders]()
			{
				return main && main->LaunchExternalGameFolders(selectedFolder,
					selectedRelativePath, supplementalFolders);
			});
			return;
		}
		const auto path = selectedTitle.localGamePath;
		std::vector<std::string> supplementalPaths;
		for (const auto& title : m_installedTitles)
		{
			if (!title.localGamePath.empty() &&
				title.isExternalStorage == selectedTitle.isExternalStorage)
				supplementalPaths.emplace_back(title.localGamePath);
		}
		BeginExternalLaunch([main, path, supplementalPaths]()
		{
			return main && main->LaunchExternalGamePath(path, supplementalPaths);
		});
		return;
	}
	const uint64_t titleId = selectedTitle.titleId;
	// Keep Windows.Gaming.Input on the XAML apartment and finish the plain
	// Cemu profile setup before the game creates its input threads.  This is
	// the Xbox/Durango-safe lifetime model: no WGI object crosses into Cemu.
	PublishGamepadStates();
	if (!m_gamepadProfileReady)
		TryConfigureDefaultGamepad();
	if (!m_gamepadProfileReady)
	{
		launchStatus->Text = "Could not prepare the Xbox Controller profile";
		AppendError("The selected Wii U controller profile was not ready before starting the game.");
		UpdateGamepadStatus();
		return;
	}
	SetTabsVisible(false);
	SetGamePresentation(true);
	startButton->IsEnabled = false;
	SetLibraryActionsEnabled(false);
	SetSystemPointerForUi(false);
	// Begin every title with the optional pointer disabled. Games that need a
	// Wii U GamePad pointer can enable it with L+R; A remains its left click.
	m_main->SetVirtualMouse(0, 0, false, false);
	// LaunchInstalledTitle() starts Cemu title threads before its task
	// continuation runs, so block profile replacement from this point onward.
	m_gameRunning = true;
	launchStatus->Text = "Mounting game, update, and DLC...";
	emulatorPlaceholder->Visibility = CollapsedValue;
	FocusEmulatorInput();
	const auto main = m_main;
	Platform::WeakReference weakThis(this);
	create_task([main, titleId]()
	{
		return main && main->LaunchInstalledTitle(titleId);
	}).then([weakThis](bool launched)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page) return;
		if (launched)
		{
			page->m_gameRunning = true;
			page->UpdateGamepadStatus();
			page->launchStatus->Text = "Game running";
			page->FocusEmulatorInput();
		}
		else
		{
			page->m_gameRunning = false;
			page->SetGamePresentation(false);
			page->SetSystemPointerForUi(true);
			page->launchStatus->Text = "Could not start the installed game";
			page->AppendError("Could not mount the base game with the installed update and DLC.");
			page->SetLibraryActionsEnabled(true);
			page->UpdateStartButton();
			page->emulatorPlaceholder->Visibility = VisibleValue;
		}
	}, task_continuation_context::use_current());
}

void DirectXPage::PerformanceMetricsCheckChanged(Platform::Object^, RoutedEventArgs^)
{
	if (m_loadingSettings || !m_main || !m_cemuReady)
		return;

	const bool show = performanceMetricsCheck->IsChecked->Value;
	if (!m_main->SetPerformanceMetrics(show))
	{
		m_loadingSettings = true;
		performanceMetricsCheck->IsChecked = m_performanceMetricsVisible;
		m_loadingSettings = false;
		AppendError("Could not change the Cemu performance metrics overlay.");
		return;
	}

	m_performanceMetricsVisible = show;
	ApplicationData::Current->LocalSettings->Values->Insert(
		WinRtString(PerformanceMetricsSettingName), PropertyValue::CreateBoolean(show));
	settingsStatus->Text = "Settings are saved automatically when changed.";
}

void DirectXPage::PlaceDimensionsFigure_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;
	const int figureIndex = dimensionsFigureBox->SelectedIndex;
	const int slot = dimensionsSlotBox->SelectedIndex;
	if (figureIndex < 0 || figureIndex >= static_cast<int>(m_dimensionsFigures.size()) ||
		slot < 0 || slot >= 7)
	{
		dimensionsStatus->Text = "Choose both a figure and a Toy Pad position.";
		return;
	}

	const auto& figure = m_dimensionsFigures[figureIndex];
	if (!m_main->PlaceDimensionsFigure(figure.id, static_cast<uint8_t>(slot)))
	{
		dimensionsStatus->Text = "The virtual tag could not be placed. Check Help and errors.";
		AppendError("Could not create or place the LEGO Dimensions virtual tag.");
		return;
	}
	static constexpr const char* slotNames[] = {
		"Left: top", "Center", "Right: top", "Left: lower 1",
		"Left: lower 2", "Right: lower 1", "Right: lower 2"
	};
	dimensionsStatus->Text = FromUtf8(
		"Placed " + figure.name + " on " + slotNames[slot] + ".");
}

void DirectXPage::RemoveDimensionsFigure_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;
	const int slot = dimensionsSlotBox->SelectedIndex;
	if (slot < 0 || slot >= 7)
	{
		dimensionsStatus->Text = "Choose a Toy Pad position first.";
		return;
	}
	if (!m_main->RemoveDimensionsFigure(static_cast<uint8_t>(slot)))
	{
		dimensionsStatus->Text = "That Toy Pad position is already empty.";
		return;
	}
	static constexpr const char* slotNames[] = {
		"Left: top", "Center", "Right: top", "Left: lower 1",
		"Left: lower 2", "Right: lower 1", "Right: lower 2"
	};
	dimensionsStatus->Text = FromUtf8(
		std::string("Removed the figure from ") + slotNames[slot] + ".");
}

void DirectXPage::MoveDimensionsFigure_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;
	const int source = dimensionsSourceSlotBox->SelectedIndex;
	const int destination = dimensionsSlotBox->SelectedIndex;
	if (source < 0 || source >= 7 || destination < 0 || destination >= 7)
	{
		dimensionsStatus->Text = "Choose source and destination Toy Pad positions.";
		return;
	}
	if (!m_main->MoveDimensionsFigure(
		static_cast<uint8_t>(source), static_cast<uint8_t>(destination)))
	{
		dimensionsStatus->Text = "The source position is empty or the tag could not be moved.";
		return;
	}
	static constexpr const char* slotNames[] = {
		"Left: top", "Center", "Right: top", "Left: lower 1",
		"Left: lower 2", "Right: lower 1", "Right: lower 2"
	};
	dimensionsStatus->Text = FromUtf8(std::string("Moved the tag from ") +
		slotNames[source] + " to " + slotNames[destination] + ".");
}

void DirectXPage::RefreshDimensionsFigures()
{
	m_dimensionsFigures.clear();
	dimensionsFigureBox->Items->Clear();
	if (!m_main || !m_cemuReady)
		return;
	m_dimensionsFigures = m_main->GetDimensionsFigures();
	for (const auto& figure : m_dimensionsFigures)
	{
		const std::string type = figure.vehicleOrGadget ? "Vehicle/Gadget" : "Character";
		dimensionsFigureBox->Items->Append(
			FromUtf8(figure.name + "  [" + type + ", " + std::to_string(figure.id) + "]"));
	}
	const bool available = !m_dimensionsFigures.empty();
	dimensionsFigureBox->IsEnabled = available;
	dimensionsSlotBox->IsEnabled = available;
	dimensionsSourceSlotBox->IsEnabled = available;
	placeDimensionsFigureButton->IsEnabled = available;
	removeDimensionsFigureButton->IsEnabled = available;
	moveDimensionsFigureButton->IsEnabled = available;
	if (available)
	{
		dimensionsFigureBox->SelectedIndex = 0;
		dimensionsStatus->Text = "Native Toy Pad ready. Virtual tags are stored in the app's local data.";
	}
	else
		dimensionsStatus->Text = "The native LEGO Dimensions catalog is unavailable.";
}

void DirectXPage::BeginInstall()
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append("*");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFolderAsync()).then(
		[weakThis](StorageFolder^ folder)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !folder) return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->startButton->IsEnabled = false;
		page->launchStatus->Text = "Installing content...";
		const auto main = page->m_main;
		create_task([main, folder]()
		{
			uint64_t baseTitleId{};
			const bool installed = main &&
				main->InstallTitle(folder, CEMU_EMBED_INSTALL_AUTO, &baseTitleId);
			return installed ? baseTitleId : uint64_t{};
		}).then([weakThis](uint64_t installedBaseTitleId)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_libraryBusy = false;
			page->SetLibraryActionsEnabled(true);
			if (!installedBaseTitleId)
			{
				page->launchStatus->Text = "Installation failed";
				page->UpdateStartButton();
				return;
			}
			page->launchStatus->Text = "Installation complete";
			page->RefreshLibrary();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::BeginExternalLaunch(std::function<bool()> launchOperation)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning || !launchOperation)
		return;
	PublishGamepadStates();
	if (!m_gamepadProfileReady)
		TryConfigureDefaultGamepad();
	if (!m_gamepadProfileReady)
	{
		launchStatus->Text = "Could not prepare the Xbox Controller profile";
		AppendError("The selected Wii U controller profile was not ready before starting the selected title.");
		return;
	}

	m_libraryBusy = true;
	SetTabsVisible(false);
	SetGamePresentation(true);
	startButton->IsEnabled = false;
	SetLibraryActionsEnabled(false);
	SetSystemPointerForUi(false);
	m_main->SetVirtualMouse(0, 0, false, false);
	m_gameRunning = true;
	launchStatus->Text = "Preparing selected Wii U title...";
	emulatorPlaceholder->Visibility = CollapsedValue;
	SetExternalLoadingVisible(true);
	FocusEmulatorInput();
	Platform::WeakReference weakThis(this);
	create_task(std::move(launchOperation)).then([weakThis](task<bool> launchTask)
	{
		bool launched = false;
		try
		{
			launched = launchTask.get();
		}
		catch (...)
		{
			launched = false;
		}
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page) return;
		page->SetExternalLoadingVisible(false);
		if (launched)
		{
			page->launchStatus->Text = "Game running";
			page->FocusEmulatorInput();
			return;
		}
		page->m_gameRunning = false;
		page->m_libraryBusy = false;
		page->SetGamePresentation(false);
		page->SetSystemPointerForUi(true);
		page->SetTabsVisible(true);
		page->launchStatus->Text = "Could not start the selected Wii U title";
		page->AppendError("Cemu could not mount or launch the selected game format.");
		page->SetLibraryActionsEnabled(true);
		page->UpdateStartButton();
		page->emulatorPlaceholder->Visibility = VisibleValue;
	}, task_continuation_context::use_current());
}

void DirectXPage::RefreshLibrary(bool scanLocalFolder)
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	m_libraryBusy = true;
	SetLibraryActionsEnabled(false);
	startButton->IsEnabled = false;
	launchStatus->Text = scanLocalFolder
		? "Scanning LocalState\\GamesToInstall..."
		: "Refreshing library...";
	const auto main = m_main;
	Platform::WeakReference weakThis(this);
	create_task([main, scanLocalFolder]()
	{
		LocalInstallScanResult scanResult{};
		try
		{
			auto importFolder = GetOrCreateLocalInstallFolder();
			if (importFolder)
			{
				std::vector<StorageFolder^> graphicPackCandidates;
				FindLocalStorageContent(importFolder, graphicPackCandidates,
					scanResult.graphicPacksAlreadyProcessed,
					scanResult.localGameFiles);
				if (main)
				{
					for (auto& gameFile : scanResult.localGameFiles)
					{
						uint64_t titleId{};
						if (main->IdentifyGamePath(gameFile.path, &titleId))
							gameFile.graphicPackTitleId = titleId;
					}
				}
				scanResult.discovered = static_cast<uint32_t>(scanResult.localGameFiles.size());
				scanResult.graphicPacksDiscovered =
					static_cast<uint32_t>(graphicPackCandidates.size());
				if (scanLocalFolder && main)
				{
					for (const auto& graphicPack : graphicPackCandidates)
					{
						uint32_t imported{};
						if (!main->InstallGraphicPacks(graphicPack, &imported) || !imported)
						{
							++scanResult.graphicPackFailures;
							continue;
						}
						scanResult.graphicPacksImported += imported;
						try
						{
							auto marker = create_task(graphicPack->CreateFileAsync(
								WinRtString(LocalGraphicPackMarkerName),
								CreationCollisionOption::ReplaceExisting)).get();
							create_task(FileIO::WriteTextAsync(marker,
								WinRtString(L"Imported by Cemu-UWP-Host. Delete this file to import this Graphic Pack again.\r\n"))).get();
						}
						catch (...)
						{
							++scanResult.markerWarnings;
						}
					}
				}
			}
		}
		catch (Platform::Exception^)
		{
			if (scanLocalFolder)
				++scanResult.failed;
		}
		catch (...)
		{
			if (scanLocalFolder)
				++scanResult.failed;
		}
		auto titles = main ? main->GetInstalledTitles() : std::vector<InstalledTitle>{};
		return std::make_pair(std::move(titles), scanResult);
	}).then([weakThis, scanLocalFolder](
		std::pair<std::vector<InstalledTitle>, LocalInstallScanResult> result)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page) return;
		auto titles = std::move(result.first);
		const auto scanResult = result.second;
		for (const auto& localGame : scanResult.localGameFiles)
		{
			InstalledTitle localTitle{};
			localTitle.titleId = localGame.id;
			localTitle.name = localGame.name;
			localTitle.regionName = "Local storage";
			localTitle.localGamePath = localGame.path;
			localTitle.localGameFormat = localGame.format;
			localTitle.graphicPackTitleId = localGame.graphicPackTitleId;
			titles.emplace_back(std::move(localTitle));
		}
		for (const auto& externalTitle : page->m_externalTitles)
			titles.emplace_back(externalTitle);
		page->m_installedTitles = std::move(titles);
		page->RefreshGraphicPackGames();
		page->installedGamesList->Items->Clear();
		for (const auto& title : page->m_installedTitles)
		{
			std::ostringstream subtitle;
			if (!title.localGamePath.empty())
			{
				subtitle << title.localGameFormat << " · "
					<< (title.isExternalStorage ? "External" : "GamesToInstall");
			}
			else
			{
				subtitle << title.regionName << " · v" << title.effectiveVersion
					<< " · Packs " << title.enabledGraphicPackCount
					<< "/" << title.compatibleGraphicPackCount;
			}

			auto item = ref new ListViewItem();
			item->HorizontalContentAlignment =
				::Windows::UI::Xaml::HorizontalAlignment::Stretch;
			item->VerticalContentAlignment =
				::Windows::UI::Xaml::VerticalAlignment::Stretch;

			auto card = ref new Border();
			card->CornerRadius = ::Windows::UI::Xaml::CornerRadius(16);
			card->BorderThickness = Thickness(1);
			card->BorderBrush = safe_cast<Brush^>(page->Resources->Lookup("DividerBrush"));
			card->Background = safe_cast<Brush^>(page->Resources->Lookup("CardBrush"));
			auto cardGrid = ref new Grid();
			auto artRow = ref new RowDefinition();
			artRow->Height = GridLength(132);
			cardGrid->RowDefinitions->Append(artRow);
			auto infoRow = ref new RowDefinition();
			infoRow->Height = GridLength(1, GridUnitType::Star);
			cardGrid->RowDefinitions->Append(infoRow);

			auto art = ref new Border();
			art->Background = safe_cast<Brush^>(page->Resources->Lookup("SageBrush"));
			art->CornerRadius = ::Windows::UI::Xaml::CornerRadius(15, 15, 0, 0);
			if (auto bitmap = DecodeGameIcon(title.iconTga))
			{
				auto image = ref new Image();
				image->Source = bitmap;
				image->Stretch = Stretch::UniformToFill;
				art->Child = image;
			}
			else
			{
				auto fallback = ref new FontIcon();
				fallback->Glyph = L"\xE7FC";
				fallback->FontSize = 46;
				fallback->Foreground = safe_cast<Brush^>(page->Resources->Lookup("AccentBlueBrush"));
				fallback->HorizontalAlignment =
					::Windows::UI::Xaml::HorizontalAlignment::Center;
				fallback->VerticalAlignment =
					::Windows::UI::Xaml::VerticalAlignment::Center;
				art->Child = fallback;
			}
			cardGrid->Children->Append(art);

			auto info = ref new StackPanel();
			Grid::SetRow(info, 1);
			info->Margin = Thickness(12, 9, 12, 9);
			auto source = ref new TextBlock();
			source->Text = title.localGamePath.empty() ? "INSTALLED" :
				(title.isExternalStorage ? "EXTERNAL STORAGE" : "LOCAL GAMES");
			source->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Consolas");
			source->FontSize = 9;
			source->Foreground = safe_cast<Brush^>(page->Resources->Lookup("AccentBlueBrush"));
			info->Children->Append(source);
			auto name = ref new TextBlock();
			name->Text = FromUtf8(title.name);
			name->FontSize = 14;
			name->FontWeight = Windows::UI::Text::FontWeights::SemiBold;
			name->Foreground = safe_cast<Brush^>(page->Resources->Lookup("TextBrush"));
			name->TextWrapping = TextWrapping::Wrap;
			name->MaxLines = 2;
			name->TextTrimming = TextTrimming::CharacterEllipsis;
			name->Margin = Thickness(0, 3, 0, 2);
			info->Children->Append(name);
			auto metadata = ref new TextBlock();
			metadata->Text = FromUtf8(subtitle.str());
			metadata->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(L"Consolas");
			metadata->FontSize = 9;
			metadata->Foreground = safe_cast<Brush^>(page->Resources->Lookup("MutedTextBrush"));
			metadata->TextTrimming = TextTrimming::CharacterEllipsis;
			info->Children->Append(metadata);
			cardGrid->Children->Append(info);
			if (title.localGamePath.empty())
			{
				auto deleteButton = ref new Button();
				deleteButton->Content = ref new Platform::String(L"×");
				deleteButton->Width = 30;
				deleteButton->Height = 30;
				deleteButton->Padding = Thickness(0);
				deleteButton->Margin = Thickness(0, 7, 7, 0);
				deleteButton->HorizontalAlignment =
					::Windows::UI::Xaml::HorizontalAlignment::Right;
				deleteButton->VerticalAlignment =
					::Windows::UI::Xaml::VerticalAlignment::Top;
				deleteButton->Background = safe_cast<Brush^>(page->Resources->Lookup("CardBrush"));
				deleteButton->IsEnabled = !page->m_gameRunning;
				const uint64_t titleId = title.titleId;
				Platform::WeakReference itemWeakThis(page);
				deleteButton->Click += ref new RoutedEventHandler(
					[itemWeakThis, titleId](Platform::Object^, RoutedEventArgs^)
					{
						auto itemPage = itemWeakThis.Resolve<DirectXPage>();
						if (itemPage)
							itemPage->DeleteInstalledTitle(titleId);
					});
				cardGrid->Children->Append(deleteButton);
			}
			Localization::Attach(cardGrid);
			card->Child = cardGrid;
			item->Content = card;
			page->installedGamesList->Items->Append(item);
		}
		const int restoredIndex = page->FindInstalledTitleIndex(page->m_selectedTitleId);
		page->installedGamesList->SelectedIndex = restoredIndex;
		if (restoredIndex < 0)
			page->m_selectedTitleId = 0;
		page->m_libraryBusy = false;
		page->SetLibraryActionsEnabled(true);
		if (scanLocalFolder)
		{
			std::ostringstream status;
			status << "Local scan complete";
			if (!scanResult.localGameFiles.empty())
				status << "; " << scanResult.localGameFiles.size()
					<< " local storage item(s) detected";
			if (!scanResult.discovered && !scanResult.graphicPacksDiscovered &&
				scanResult.localGameFiles.empty())
				status << "; no new content found";
			if (scanResult.graphicPacksImported)
				status << "; " << scanResult.graphicPacksImported << " Graphic Pack(s) imported";
			if (scanResult.graphicPacksAlreadyProcessed)
				status << "; " << scanResult.graphicPacksAlreadyProcessed << " Graphic Pack(s) already processed";
			if (scanResult.failed)
			{
				status << "; " << scanResult.failed << " failed";
				page->AppendError("Some content in LocalState\\GamesToInstall could not be scanned. Each source must be a supported game file, a title.tmd folder, or an extracted title with code, content, and meta folders.");
			}
			if (scanResult.graphicPackFailures)
			{
				status << "; " << scanResult.graphicPackFailures << " Graphic Pack import(s) failed";
				page->AppendError("Some Graphic Packs in LocalState\\GamesToInstall could not be imported. Each pack folder must contain a valid rules.txt file.");
			}
			if (scanResult.markerWarnings)
			{
				status << "; " << scanResult.markerWarnings << " marker warning(s)";
				page->AppendError("Installed content could not be marked as processed and may be detected again on the next scan.");
			}
			page->launchStatus->Text = FromUtf8(status.str());
		}
		else
		{
			if (!page->m_externalTitles.empty())
			{
				page->launchStatus->Text = FromUtf8("External scan complete; " +
					std::to_string(page->m_externalTitles.size()) +
					" supported title item(s) ready to launch from external storage");
			}
			else
			{
				page->launchStatus->Text = page->m_installedTitles.empty()
					? "No games installed"
					: (restoredIndex >= 0 ? "Ready to start" : "Select an installed game");
			}
		}
		page->UpdateStartButton();
	}, task_continuation_context::use_current());
}

void DirectXPage::SetLibraryActionsEnabled(bool enabled)
{
	const bool canUse = enabled && m_cemuReady;
	installContentButton->IsEnabled = canUse;
	importKeysButton->IsEnabled = canUse && !m_gameRunning;
	refreshLibraryButton->IsEnabled = canUse;
	scanExternalStorageButton->IsEnabled = canUse && !m_gameRunning;
	rescanExternalStorageButton->IsEnabled = canUse && !m_gameRunning;
	forgetExternalStorageButton->IsEnabled = canUse && !m_gameRunning;
	downloadGraphicPacksButton->IsEnabled = canUse && !m_gameRunning;
	installGraphicPacksButton->IsEnabled = canUse && !m_gameRunning;
	installGraphicPacksTabButton->IsEnabled = canUse && !m_gameRunning;
	downloadGraphicPacksTabButton->IsEnabled = canUse && !m_gameRunning;
	graphicPackGameBox->IsEnabled = canUse && !m_gameRunning &&
		!m_graphicPackGameIds.empty();
	clearShaderCacheButton->IsEnabled = canUse && !m_gameRunning;
	installedGamesList->IsEnabled = enabled;
}

void DirectXPage::NavigationButton_Click(Platform::Object^ sender, RoutedEventArgs^)
{
	auto element = dynamic_cast<FrameworkElement^>(sender);
	auto tag = element ? dynamic_cast<Platform::String^>(element->Tag) : nullptr;
	if (!tag || tag->IsEmpty())
		return;
	const int index = static_cast<int>(wcstol(tag->Data(), nullptr, 10));
	if (index < 0 || index >= static_cast<int>(toolTabs->Items->Size))
		return;
	toolTabs->SelectedIndex = index;
	SetTabsVisible(true);
}

void DirectXPage::ToolTabs_SelectionChanged(
	Platform::Object^, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^)
{
	static const wchar_t* tabNames[] = {
		L"Home", L"Library", L"Toy Pad", L"Graphic packs", L"Settings", L"Help and errors"
	};
	const int selectedIndex = toolTabs->SelectedIndex;
	std::array<Button^, 6> navigationButtons{
		homeNavigationButton, libraryNavigationButton, toyPadNavigationButton,
		graphicPacksNavigationButton, settingsNavigationButton, helpNavigationButton
	};
	auto transparent = ref new SolidColorBrush(Windows::UI::Colors::Transparent);
	auto selectedBackground = safe_cast<Brush^>(Resources->Lookup("AccentBlueBrush"));
	auto normalForeground = safe_cast<Brush^>(Resources->Lookup("TextBrush"));
	auto selectedForeground = ref new SolidColorBrush(Windows::UI::Colors::White);
	for (size_t buttonIndex = 0; buttonIndex < navigationButtons.size(); ++buttonIndex)
	{
		const bool selected = buttonIndex == static_cast<size_t>(selectedIndex);
		navigationButtons[buttonIndex]->Background = selected ? selectedBackground : transparent;
		navigationButtons[buttonIndex]->BorderBrush = selected ? selectedBackground : transparent;
		navigationButtons[buttonIndex]->Foreground = selected ? selectedForeground : normalForeground;
	}
	if (currentTabStatus && selectedIndex >= 0 &&
		selectedIndex < static_cast<int>(sizeof(tabNames) / sizeof(tabNames[0])))
		currentTabStatus->Text = WinRtString(tabNames[selectedIndex]);
	// Every tool page owns the complete area below the command bar. Individual
	// pages provide their own scrolling where their content exceeds that area.
	// Recalculate card width after Pivot finishes changing pages.
	if (toolTabs->SelectedIndex == 3)
		UpdateGraphicPackGridColumns();
}

void DirectXPage::LoadSettings()
{
	CemuEmbedSettings settings{};
	if (!m_main || !m_main->GetSettings(settings))
	{
		settingsStatus->Text = "Cemu settings are unavailable.";
		return;
	}
	m_loadingSettings = true;
	LoadGlobalGamepadMapping();
	auto select = [](ComboBox^ box, int value, int maximum)
	{
		box->SelectedIndex = (std::max)(0, (std::min)(value, maximum));
	};
	select(cpuModeBox, settings.cpu_mode, 4);
	select(consoleLanguageBox, settings.console_language, 11);
	select(emulatedControllerBox, settings.emulated_controller_type, 3);
	// The UWP host exposes one supported graphics path. Normalize settings from
	// older builds so an unsupported saved value cannot remain active.
	if (settings.graphics_api != 3)
	{
		settings.graphics_api = 3;
		m_main->SetSettings(settings);
	}
	rendererMetricsText->Text = "Renderer         D3D11";
	select(vsyncBox, settings.vsync == 0 ? 0 : 1, 1);
	bootSoundCheck->IsChecked = settings.play_boot_sound != 0;
	disableScreensaverCheck->IsChecked = settings.disable_screensaver != 0;
	asyncCompileCheck->IsChecked = settings.async_compile != 0;
	gx2SyncCheck->IsChecked = settings.gx2drawdone_sync != 0;
	upsideDownCheck->IsChecked = settings.render_upside_down != 0;
	select(upscaleFilterBox, settings.upscale_filter, 3);
	select(downscaleFilterBox, settings.downscale_filter, 3);
	select(scalingBox, settings.fullscreen_scaling, 1);
	overrideGammaCheck->IsChecked = settings.override_gamma != 0;
	gammaSlider->Value = settings.override_gamma_value;
	displayGammaSlider->Value = settings.display_gamma;
	select(notificationPositionBox, settings.notification_position, 6);
	notificationScaleSlider->Value = settings.notification_text_scale;
	notifyProfilesCheck->IsChecked = settings.notification_controller_profiles != 0;
	notifyBatteryCheck->IsChecked = settings.notification_controller_battery != 0;
	notifyShadersCheck->IsChecked = settings.notification_shader_compiling != 0;
	notifyFriendsCheck->IsChecked = settings.notification_friends != 0;
	// The UI order is optimized for UWP, but settings.xml stores IAudioAPI's
	// native enum values: DirectSound=0, XAudio27=1, XAudio2=2, Cubeb=3.
	const int audioApiIndex = settings.audio_api == 0 ? 1 :
		settings.audio_api == 1 ? 2 :
		settings.audio_api == 3 ? 3 : 0;
	select(audioApiBox, audioApiIndex, 3);
	audioDelaySlider->Value = settings.audio_delay;
	select(tvChannelsBox, settings.tv_channels, 2);
	select(padChannelsBox, settings.pad_channels, 2);
	select(inputChannelsBox, settings.input_channels, 2);
	tvVolumeSlider->Value = settings.tv_volume;
	padVolumeSlider->Value = settings.pad_volume;
	inputVolumeSlider->Value = settings.input_volume;
	portalVolumeSlider->Value = settings.portal_volume;
	skylandersCheck->IsChecked = settings.emulate_skylander_portal != 0;
	infinityCheck->IsChecked = settings.emulate_infinity_base != 0;
	dimensionsCheck->IsChecked = settings.emulate_dimensions_toypad != 0;
	bool showMetrics = false;
	auto values = ApplicationData::Current->LocalSettings->Values;
	if (values->HasKey(WinRtString(PerformanceMetricsSettingName)))
	{
		auto property = dynamic_cast<IPropertyValue^>(
			values->Lookup(WinRtString(PerformanceMetricsSettingName)));
		if (property && property->Type == PropertyType::Boolean)
			showMetrics = property->GetBoolean();
	}
	performanceMetricsCheck->IsChecked = showMetrics;
	if (m_main->SetPerformanceMetrics(showMetrics))
		m_performanceMetricsVisible = showMetrics;
	m_loadingSettings = false;
	settingsStatus->Text = "Settings are saved automatically when changed.";
}

void DirectXPage::SettingsSelectionChanged(Platform::Object^,
	SelectionChangedEventArgs^)
{
	SaveSettings();
}

void DirectXPage::SettingsCheckChanged(Platform::Object^, RoutedEventArgs^)
{
	SaveSettings();
}

void DirectXPage::SettingsValueChanged(Platform::Object^,
	Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^)
{
	SaveSettings();
}

void DirectXPage::SaveSettings()
{
	if (m_loadingSettings)
		return;
	if (!m_main || !m_cemuReady || m_gameRunning)
	{
		if (m_gameRunning)
			settingsStatus->Text = "Stop the running game before changing settings.";
		return;
	}
	CemuEmbedSettings settings{};
	if (!m_main->GetSettings(settings))
	{
		settingsStatus->Text = "Could not read the current Cemu settings.";
		return;
	}
	auto checked = [](CheckBox^ box) { return box->IsChecked->Value ? 1 : 0; };
	settings.cpu_mode = cpuModeBox->SelectedIndex;
	settings.console_language = consoleLanguageBox->SelectedIndex;
	settings.emulated_controller_type = emulatedControllerBox->SelectedIndex;
	settings.graphics_api = 3;
	settings.vsync = vsyncBox->SelectedIndex;
	settings.play_boot_sound = checked(bootSoundCheck);
	settings.disable_screensaver = checked(disableScreensaverCheck);
	settings.async_compile = checked(asyncCompileCheck);
	settings.gx2drawdone_sync = checked(gx2SyncCheck);
	settings.render_upside_down = checked(upsideDownCheck);
	settings.upscale_filter = upscaleFilterBox->SelectedIndex;
	settings.downscale_filter = downscaleFilterBox->SelectedIndex;
	settings.fullscreen_scaling = scalingBox->SelectedIndex;
	settings.override_gamma = checked(overrideGammaCheck);
	settings.override_gamma_value = static_cast<float>(gammaSlider->Value);
	settings.display_gamma = static_cast<float>(displayGammaSlider->Value);
	settings.notification_position = notificationPositionBox->SelectedIndex;
	settings.notification_text_scale = static_cast<int32_t>(notificationScaleSlider->Value);
	settings.notification_controller_profiles = checked(notifyProfilesCheck);
	settings.notification_controller_battery = checked(notifyBatteryCheck);
	settings.notification_shader_compiling = checked(notifyShadersCheck);
	settings.notification_friends = checked(notifyFriendsCheck);
	static constexpr int32_t audioApiValues[] = { 2, 0, 1, 3 };
	const int audioApiIndex = (std::max)(0,
		(std::min)(audioApiBox->SelectedIndex, 3));
	settings.audio_api = audioApiValues[audioApiIndex];
	settings.audio_delay = static_cast<int32_t>(audioDelaySlider->Value);
	settings.tv_channels = tvChannelsBox->SelectedIndex;
	settings.pad_channels = padChannelsBox->SelectedIndex;
	settings.input_channels = inputChannelsBox->SelectedIndex;
	settings.tv_volume = static_cast<int32_t>(tvVolumeSlider->Value);
	settings.pad_volume = static_cast<int32_t>(padVolumeSlider->Value);
	settings.input_volume = static_cast<int32_t>(inputVolumeSlider->Value);
	settings.portal_volume = static_cast<int32_t>(portalVolumeSlider->Value);
	settings.emulate_skylander_portal = checked(skylandersCheck);
	settings.emulate_infinity_base = checked(infinityCheck);
	settings.emulate_dimensions_toypad = checked(dimensionsCheck);
	if (!m_main->SetSettings(settings))
	{
		settingsStatus->Text = "Could not save Cemu settings.";
		return;
	}
	SaveGlobalGamepadMapping();
	TryConfigureDefaultGamepad();
	UpdateGamepadStatus();
	settingsStatus->Text = "Settings saved automatically. USB and other startup options apply after restarting the app.";
}

void DirectXPage::ClearErrors_Click(Platform::Object^, RoutedEventArgs^)
{
	errorsList->Items->Clear();
}

void DirectXPage::EmulatorViewport_PointerPressed(
	Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
	if (!m_gameRunning)
		return;
	FocusEmulatorInput();
}

void DirectXPage::FocusEmulatorInput()
{
	// SwapChainPanel is not a Control and cannot own XAML focus. The Page is a
	// focusable Control, so focusing it releases ListView/AppBarButton focus
	// while CoreWindow, SDL3-UWP and Windows.Gaming.Input continue receiving
	// keyboard and controller input for Cemu.
	startButton->IsTabStop = false;
	this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void DirectXPage::SetSystemPointerForUi(bool enabled)
{
	// Application.RequiresPointerMode can only be established during app
	// startup on this UWP runtime. CoreWindow.PointerCursor is the supported
	// runtime switch for hiding and restoring the controller-driven cursor.
	if (enabled && !m_systemPointerHidden)
		return;
	try
	{
		auto window = Window::Current;
		if (!window || !window->CoreWindow)
			return;
		auto coreWindow = window->CoreWindow;
		if (enabled)
		{
			coreWindow->PointerCursor = m_savedSystemPointerCursor;
			m_savedSystemPointerCursor = nullptr;
			m_systemPointerHidden = false;
		}
		else
		{
			if (!m_systemPointerHidden)
				m_savedSystemPointerCursor = coreWindow->PointerCursor;
			if (coreWindow->PointerCursor != nullptr)
				coreWindow->PointerCursor = nullptr;
			m_systemPointerHidden = true;
		}
	}
	catch (Platform::Exception^)
	{
		// Cursor visibility must never interrupt launch or shutdown.
	}
}

void DirectXPage::EmulatorViewport_SizeChanged(Platform::Object^, SizeChangedEventArgs^ args)
{
	if (args->NewSize.Width <= 0 || args->NewSize.Height <= 0) return;
	if (!m_main)
	{
		InitializeEmulator(args->NewSize.Width, args->NewSize.Height);
		return;
	}
	UpdateEmulatorSurfaceSize(args->NewSize.Width, args->NewSize.Height);
}

void DirectXPage::EmulatorSurface_CompositionScaleChanged(
	SwapChainPanel^ sender, Platform::Object^)
{
	if (!sender)
		return;
	UpdateEmulatorSurfaceSize(
		static_cast<float>(sender->ActualWidth),
		static_cast<float>(sender->ActualHeight));
}

void DirectXPage::UpdateEmulatorSurfaceSize(float width, float height)
{
	if (!m_main || width <= 0.0f || height <= 0.0f)
		return;
	// ResizeBuffers is performed by Cemu's D3D11 presentation thread, where all
	// back-buffer references can be released safely. The XAML thread only
	// publishes the new composition-pixel size.
	double scaleX = emulatorSurface->CompositionScaleX;
	double scaleY = emulatorSurface->CompositionScaleY;
	if (scaleX <= 0.0 || scaleY <= 0.0)
	{
		const auto dpiScale =
			Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->LogicalDpi / 96.0;
		scaleX = scaleY = dpiScale;
	}
	m_main->ResizeSurface(
		static_cast<int>(std::lround(width * scaleX)),
		static_cast<int>(std::lround(height * scaleY)),
		(scaleX + scaleY) * 0.5);
}

void DirectXPage::SetTabsVisible(bool visible)
{
	// While a title owns the surface, no XAML chrome may cover or resize it.
	// Options become available again only after leaving the game presentation.
	if (m_gameRunning && visible)
		visible = false;
	tabsPanel->Visibility = visible ? VisibleValue : CollapsedValue;
	if (!visible && m_gameRunning)
		FocusEmulatorInput();
}

void DirectXPage::SetGamePresentation(bool running)
{
	if (running)
	{
		// Give the running title the complete client area. This is a layout change
		// inside the existing Xbox/UWP window, not a fullscreen-mode transition.
		navigationRail->Visibility = CollapsedValue;
		commandFooter->Visibility = CollapsedValue;
		Grid::SetColumn(stageGrid, 0);
		Grid::SetColumnSpan(stageGrid, 2);
		Grid::SetRow(emulatorViewport, 0);
		Grid::SetRowSpan(emulatorViewport, 2);
		tabsPanel->Visibility = CollapsedValue;
		return;
	}

	navigationRail->Visibility = VisibleValue;
	commandFooter->Visibility = VisibleValue;
	Grid::SetColumn(stageGrid, 1);
	Grid::SetColumnSpan(stageGrid, 1);
	Grid::SetRow(emulatorViewport, 0);
	Grid::SetRowSpan(emulatorViewport, 2);
	// Restore every focus target disabled while the title owned input.
	startButton->IsTabStop = true;
	tabsPanel->Visibility = VisibleValue;
}

void DirectXPage::SetExternalLoadingVisible(bool visible)
{
	m_externalLoadingVisible = visible;
	externalLoadingOverlay->Visibility = visible ? VisibleValue : CollapsedValue;
	externalLoadingRing->IsActive = visible;
	if (!visible)
		return;

	externalLoadingProgress->Value = 0.0;
	externalLoadingProgress->IsIndeterminate = false;
	externalLoadingPercent->Text = "0%";
	externalLoadingTitle->Text = "Loading external game";
	externalLoadingDetail->Text = "Preparing external storage...";
}

void DirectXPage::AppendError(const std::string& message)
{
	constexpr unsigned int maxRetainedErrors = 50;
	auto text = FromUtf8(message);
	if (Dispatcher->HasThreadAccess)
	{
		errorsList->Items->Append(Localization::Get(text));
		while (errorsList->Items->Size > maxRetainedErrors)
			errorsList->Items->RemoveAt(0);
		return;
	}
	Platform::WeakReference weakThis(this);
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([weakThis, text, maxRetainedErrors]()
		{
			if (auto page = weakThis.Resolve<DirectXPage>())
			{
				page->errorsList->Items->Append(Localization::Get(text));
				while (page->errorsList->Items->Size > maxRetainedErrors)
					page->errorsList->Items->RemoveAt(0);
			}
		})));
}

void DirectXPage::OnCemuStateChanged(CemuEmbedState state)
{
	Platform::WeakReference weakThis(this);
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([weakThis, state]()
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_cemuReady = state == CEMU_EMBED_STATE_READY;
			if (state == CEMU_EMBED_STATE_READY)
			{
				page->launchStatus->Text = "Loading library...";
				page->SetLibraryActionsEnabled(true);
				page->m_gamepadRetryFrames = 59;
				page->UpdateActiveAccount();
				page->TryConfigureDefaultGamepad();
				page->RestoreExternalStorageFolders();
				page->RefreshDimensionsFigures();
				page->LoadSettings();
			}
			else if (state == CEMU_EMBED_STATE_INITIALIZING)
				page->launchStatus->Text = "Initializing emulator...";
			else if (state == CEMU_EMBED_STATE_FAILED)
			{
				page->launchStatus->Text = "Failed to initialize the emulator";
				page->accountStatus->Text = "Account unavailable";
				page->accountStatusIcon->Opacity = 0.45;
			}
			if (state != CEMU_EMBED_STATE_READY)
			{
				page->m_gameRunning = false;
				page->SetExternalLoadingVisible(false);
				page->SetGamePresentation(false);
				page->SetSystemPointerForUi(true);
				if (page->m_virtualMouseEnabled)
					page->SetVirtualMouseEnabled(false);
				page->SetLibraryActionsEnabled(false);
				page->dimensionsFigureBox->IsEnabled = false;
				page->dimensionsSlotBox->IsEnabled = false;
				page->dimensionsSourceSlotBox->IsEnabled = false;
				page->placeDimensionsFigureButton->IsEnabled = false;
				page->removeDimensionsFigureButton->IsEnabled = false;
				page->moveDimensionsFigureButton->IsEnabled = false;
			}
			page->UpdateStartButton();
		})));
}

void DirectXPage::SetVirtualMouseEnabled(bool enabled)
{
	if (m_virtualMouseEnabled == enabled)
		return;

	m_virtualMouseEnabled = enabled;
	m_virtualMouseLeftDown = false;
	m_virtualMouseLastUpdate = std::chrono::steady_clock::now();

	if (enabled)
	{
		const double width = emulatorViewport->ActualWidth;
		const double height = emulatorViewport->ActualHeight;
		if (m_virtualMouseX <= 0.0 || m_virtualMouseY <= 0.0)
		{
			m_virtualMouseX = width * 0.5;
			m_virtualMouseY = height * 0.5;
		}
		m_virtualMouseX = (std::max)(0.0, (std::min)(m_virtualMouseX, width));
		m_virtualMouseY = (std::max)(0.0, (std::min)(m_virtualMouseY, height));
		virtualMouseTransform->X = m_virtualMouseX;
		virtualMouseTransform->Y = m_virtualMouseY;
		virtualMouseCursor->Visibility = VisibleValue;

		const double scaleX = emulatorSurface->CompositionScaleX > 0.0
			? emulatorSurface->CompositionScaleX : 1.0;
		const double scaleY = emulatorSurface->CompositionScaleY > 0.0
			? emulatorSurface->CompositionScaleY : 1.0;
		if (m_main)
			m_main->SetVirtualMouse(
				static_cast<int>(std::lround(m_virtualMouseX * scaleX)),
				static_cast<int>(std::lround(m_virtualMouseY * scaleY)), false, true);
	}
	else
	{
		if (m_main)
			m_main->SetVirtualMouse(0, 0, false, false);
		virtualMouseCursor->Visibility = CollapsedValue;
	}

	UpdateGamepadStatus();
}

void DirectXPage::UpdateVirtualMouse(const CemuEmbedGamepadState& gamepad)
{
	if (!m_gameRunning || !m_gamepadProfileReady || !gamepad.connected ||
		tabsPanel->Visibility == VisibleValue)
	{
		m_virtualMouseChordHeld = false;
		if (m_virtualMouseEnabled)
			SetVirtualMouseEnabled(false);
		return;
	}

	constexpr uint32_t leftShoulder = 1u << 9;
	constexpr uint32_t rightShoulder = 1u << 10;
	constexpr uint32_t buttonA = 1u << 0;
	const bool chordPressed =
		(gamepad.buttons & leftShoulder) != 0 &&
		(gamepad.buttons & rightShoulder) != 0;
	if (chordPressed && !m_virtualMouseChordHeld)
		SetVirtualMouseEnabled(!m_virtualMouseEnabled);
	m_virtualMouseChordHeld = chordPressed;

	const auto now = std::chrono::steady_clock::now();
	if (!m_virtualMouseEnabled)
	{
		m_virtualMouseLastUpdate = now;
		return;
	}

	double elapsed = std::chrono::duration<double>(now - m_virtualMouseLastUpdate).count();
	m_virtualMouseLastUpdate = now;
	elapsed = (std::max)(0.0, (std::min)(elapsed, 0.05));
	constexpr double cursorSpeed = 900.0;
	m_virtualMouseX += ApplyStickDeadzone(gamepad.left_x) * cursorSpeed * elapsed;
	m_virtualMouseY -= ApplyStickDeadzone(gamepad.left_y) * cursorSpeed * elapsed;

	const double width = emulatorViewport->ActualWidth;
	const double height = emulatorViewport->ActualHeight;
	m_virtualMouseX = (std::max)(0.0, (std::min)(m_virtualMouseX, width));
	m_virtualMouseY = (std::max)(0.0, (std::min)(m_virtualMouseY, height));
	virtualMouseTransform->X = m_virtualMouseX;
	virtualMouseTransform->Y = m_virtualMouseY;
	m_virtualMouseLeftDown = (gamepad.buttons & buttonA) != 0;

	const double scaleX = emulatorSurface->CompositionScaleX > 0.0
		? emulatorSurface->CompositionScaleX : 1.0;
	const double scaleY = emulatorSurface->CompositionScaleY > 0.0
		? emulatorSurface->CompositionScaleY : 1.0;
	if (m_main)
		m_main->SetVirtualMouse(
			static_cast<int>(std::lround(m_virtualMouseX * scaleX)),
			static_cast<int>(std::lround(m_virtualMouseY * scaleY)),
			m_virtualMouseLeftDown, true);
}

void DirectXPage::UpdateGamepadStatus()
{
	const wchar_t* controllerTypes[] = {
		L"Wii U GamePad", L"Wii U Pro Controller", L"Wii Classic Controller", L"Wii Remote"
	};
	const int playerOneType = (std::max)(0, (std::min)(emulatedControllerBox->SelectedIndex, 3));
	std::array<TextBlock^, CEMU_EMBED_MAX_GAMEPADS> rows = {
		player1ControllerStatus, player2ControllerStatus,
		player3ControllerStatus, player4ControllerStatus
	};
	for (size_t index = 0; index < rows.size(); ++index)
	{
		const bool connected = m_gamepads[index] != nullptr;
		auto type = Localization::GetLiteral(index == 0
			? controllerTypes[playerOneType] : L"Wii U Pro Controller");
		auto state = Localization::GetLiteral(connected ? L"Connected" : L"Disconnected");
		Platform::String^ name = nullptr;
		if (!m_gamepadNames[index].empty())
			name = ref new Platform::String(m_gamepadNames[index].c_str());
		else if (connected)
			name = Localization::GetLiteral(L"Xbox controller");
		else
			name = WinRtString(L"\u2014");
		std::wostringstream row;
		row << L"P" << (index + 1) << L"  \u00B7  " << name->Data()
			<< L"  \u00B7  " << type->Data() << L"  \u00B7  " << state->Data();
		rows[index]->Text = ref new Platform::String(row.str().c_str());
		rows[index]->Opacity = connected ? 1.0 : 0.65;
	}

	size_t connectedCount = 0;
	for (const auto gamepad : m_gamepads)
		if (gamepad != nullptr)
			++connectedCount;
	if (connectedCount == 0)
	{
		controllerStatus->Text = "Controller disconnected";
		controllerStatus->Opacity = 0.65;
		controllerStatusIcon->Opacity = 0.45;
		return;
	}
	std::wostringstream text;
	text << connectedCount << (connectedCount == 1 ? L" controller connected" : L" controllers connected");
	if (!m_gamepadProfileReady)
		text << L" \u2022 preparing";
	else if (m_virtualMouseEnabled)
		text << L" \u2022 mouse";
	controllerStatus->Text = ref new Platform::String(text.str().c_str());
	controllerStatus->Opacity = 1.0;
	controllerStatusIcon->Opacity = 1.0;
}

void DirectXPage::UpdateGamepadVibration()
{
	if (!m_main)
		return;
	for (uint32_t playerIndex = 0; playerIndex < CEMU_EMBED_MAX_GAMEPADS; ++playerIndex)
	{
		auto gamepad = m_gamepads[playerIndex];
		if (!gamepad)
		{
			m_lastAppliedGamepadRumble[playerIndex] = -1.0f;
			continue;
		}

		if (!m_gameRunning)
		{
			if (m_lastAppliedGamepadRumble[playerIndex] != 0.0f)
				StopGamepadVibration(gamepad);
			m_lastAppliedGamepadRumble[playerIndex] = 0.0f;
			continue;
		}

		float requested = 0.0f;
		if (!m_main->GetGamepadRumble(playerIndex, requested))
			continue;
		requested = (std::max)(0.0f, (std::min)(requested, 1.0f));
		if (std::abs(requested - m_lastAppliedGamepadRumble[playerIndex]) < 0.001f)
			continue;
		try
		{
			GamepadVibration vibration;
			vibration.LeftMotor = requested;
			vibration.RightMotor = requested;
			vibration.LeftTrigger = 0.0;
			vibration.RightTrigger = 0.0;
			gamepad->Vibration = vibration;
			m_lastAppliedGamepadRumble[playerIndex] = requested;
		}
		catch (Platform::Exception^)
		{
			m_lastAppliedGamepadRumble[playerIndex] = -1.0f;
		}
	}
}

std::array<ComboBox^, 16> DirectXPage::GetGlobalGamepadBindingBoxes() const
{
	return {
		gamepadMapABox, gamepadMapBBox, gamepadMapXBox, gamepadMapYBox,
		gamepadMapLBox, gamepadMapRBox, gamepadMapZLBox, gamepadMapZRBox,
		gamepadMapPlusBox, gamepadMapMinusBox, gamepadMapUpBox, gamepadMapDownBox,
		gamepadMapLeftBox, gamepadMapRightBox, gamepadMapStickLBox, gamepadMapStickRBox
	};
}

void DirectXPage::InitializeGlobalGamepadMappingUi()
{
	static const wchar_t* sourceNames[16] = {
		L"Xbox A", L"Xbox B", L"Xbox X", L"Xbox Y",
		L"LB", L"RB", L"LT", L"RT",
		L"Menu", L"View",
		L"D-pad Up", L"D-pad Down", L"D-pad Left", L"D-pad Right",
		L"Left stick click", L"Right stick click"
	};
	for (auto box : GetGlobalGamepadBindingBoxes())
	{
		if (!box || box->Items->Size != 0)
			continue;
		for (const auto name : sourceNames)
			box->Items->Append(WinRtString(name));
	}
}

void DirectXPage::LoadGlobalGamepadMapping()
{
	auto values = ApplicationData::Current->LocalSettings->Values;
	auto boxes = GetGlobalGamepadBindingBoxes();
	for (size_t index = 0; index < boxes.size(); ++index)
	{
		int32_t binding = kDefaultGlobalGamepadBindings[index];
		const std::wstring key = L"GlobalGamepadBinding" + std::to_wstring(index);
		auto keyString = ref new Platform::String(key.c_str());
		if (values->HasKey(keyString))
		{
			auto property = dynamic_cast<IPropertyValue^>(values->Lookup(keyString));
			if (property && property->Type == PropertyType::Int32)
			{
				const int32_t stored = property->GetInt32();
				if (stored >= 0 && stored < 16)
					binding = stored;
			}
		}
		m_globalGamepadBindings[index] = binding;
		boxes[index]->SelectedIndex = binding;
	}
}

void DirectXPage::SaveGlobalGamepadMapping()
{
	auto values = ApplicationData::Current->LocalSettings->Values;
	auto boxes = GetGlobalGamepadBindingBoxes();
	for (size_t index = 0; index < boxes.size(); ++index)
	{
		const int32_t binding = boxes[index] && boxes[index]->SelectedIndex >= 0
			? boxes[index]->SelectedIndex : kDefaultGlobalGamepadBindings[index];
		m_globalGamepadBindings[index] = binding;
		const std::wstring key = L"GlobalGamepadBinding" + std::to_wstring(index);
		values->Insert(ref new Platform::String(key.c_str()),
			PropertyValue::CreateInt32(binding));
	}
}

void DirectXPage::ResetGamepadMapping_Click(Platform::Object^, RoutedEventArgs^)
{
	if (m_gameRunning)
	{
		settingsStatus->Text = "Stop the running game before changing the button layout.";
		return;
	}
	const bool wasLoading = m_loadingSettings;
	m_loadingSettings = true;
	auto boxes = GetGlobalGamepadBindingBoxes();
	for (size_t index = 0; index < boxes.size(); ++index)
		boxes[index]->SelectedIndex = kDefaultGlobalGamepadBindings[index];
	m_loadingSettings = wasLoading;
	if (!wasLoading)
	{
		SaveGlobalGamepadMapping();
		settingsStatus->Text = "Default global button layout restored for P1-P4.";
	}
}

CemuEmbedGamepadState DirectXPage::ApplyGlobalGamepadMapping(
	const CemuEmbedGamepadState& raw) const
{
	if (!raw.connected)
		return raw;

	auto sourceValue = [&raw](int32_t source) -> float
	{
		if (source == 6)
			return raw.left_trigger;
		if (source == 7)
			return raw.right_trigger;
		static const uint32_t sourceBits[16] =
			{ 0, 1, 2, 3, 9, 10, 0, 0, 6, 4, 11, 12, 13, 14, 7, 8 };
		if (source < 0 || source >= 16)
			return 0.0f;
		return (raw.buttons & (1u << sourceBits[source])) != 0 ? 1.0f : 0.0f;
	};

	CemuEmbedGamepadState mapped = raw;
	mapped.buttons = 0;
	auto setButton = [&](size_t binding, uint32_t targetBit)
	{
		if (sourceValue(m_globalGamepadBindings[binding]) >= 0.5f)
			mapped.buttons |= 1u << targetBit;
	};

	// Canonical WGI slots consumed by Cemu's VPAD/Pro/Classic mappings.
	setButton(0, 1);  // Wii U A
	setButton(1, 0);  // Wii U B
	setButton(2, 3);  // Wii U X
	setButton(3, 2);  // Wii U Y
	setButton(4, 9);  // L
	setButton(5, 10); // R
	mapped.left_trigger = sourceValue(m_globalGamepadBindings[6]);
	mapped.right_trigger = sourceValue(m_globalGamepadBindings[7]);
	setButton(8, 6);  // Plus / Menu
	setButton(9, 4);  // Minus / View
	setButton(10, 11);
	setButton(11, 12);
	setButton(12, 13);
	setButton(13, 14);
	setButton(14, 7);
	setButton(15, 8);
	return mapped;
}

CemuEmbedGamepadState DirectXPage::PublishGamepadStates()
{
	CemuEmbedGamepadState primaryState{};
	primaryState.struct_size = sizeof(primaryState);
	primaryState.abi_version = CEMU_EMBED_GAMEPAD_VERSION;
	if (!m_main)
		return primaryState;

	for (uint32_t playerIndex = 0; playerIndex < CEMU_EMBED_MAX_GAMEPADS; ++playerIndex)
	{
		CemuEmbedGamepadState state{};
		state.struct_size = sizeof(state);
		state.abi_version = CEMU_EMBED_GAMEPAD_VERSION;
		auto gamepad = m_gamepads[playerIndex];
		if (gamepad)
		{
			try
			{
				auto reading = gamepad->GetCurrentReading();
				state.connected = 1;
				state.buttons = NormalizeGamepadButtons(reading.Buttons);
				state.left_x = static_cast<float>(reading.LeftThumbstickX);
				state.left_y = static_cast<float>(reading.LeftThumbstickY);
				state.right_x = static_cast<float>(reading.RightThumbstickX);
				state.right_y = static_cast<float>(reading.RightThumbstickY);
				state.left_trigger = static_cast<float>(reading.LeftTrigger);
				state.right_trigger = static_cast<float>(reading.RightTrigger);
			}
			catch (Platform::Exception^)
			{
				// Xbox can revoke a user-device association while the removal event
				// is still queued. Drop only that slot and keep the other players.
				DisconnectGamepadSlot(playerIndex);
			}
		}

		if (playerIndex == 0)
			primaryState = state;

		// Do not let UI navigation also control the running Wii U title. Keep
		// every device connected, but first apply the single host-owned gameplay
		// layout shared by P1-P4. primaryState above deliberately remains raw so
		// host UI shortcuts and the virtual mouse never inherit gameplay remaps.
		auto publishedState = ApplyGlobalGamepadMapping(state);
		// Publish neutral readings while options own
		// controller navigation.
		if (m_gameRunning && tabsPanel->Visibility == VisibleValue)
		{
			publishedState.buttons = 0;
			publishedState.left_x = 0.0f;
			publishedState.left_y = 0.0f;
			publishedState.right_x = 0.0f;
			publishedState.right_y = 0.0f;
			publishedState.left_trigger = 0.0f;
			publishedState.right_trigger = 0.0f;
		}
		if (!m_hasPublishedGamepadStates[playerIndex] ||
			std::memcmp(&publishedState, &m_lastPublishedGamepadStates[playerIndex],
				sizeof(publishedState)) != 0)
		{
			m_main->SetGamepadState(playerIndex, publishedState);
			m_lastPublishedGamepadStates[playerIndex] = publishedState;
			m_hasPublishedGamepadStates[playerIndex] = true;
		}
	}
	return primaryState;
}

void DirectXPage::UpdateActiveAccount()
{
	ActiveAccount account;
	if (!m_main || !m_main->GetActiveAccount(account))
	{
		accountStatus->Text = "Account unavailable";
		accountStatus->Opacity = 0.65;
		accountStatusIcon->Opacity = 0.45;
		return;
	}

	std::ostringstream text;
	text << (account.miiName.empty() ? "Default account" : account.miiName);
	if (account.onlineEnabled)
		text << " \xE2\x80\xA2 online";
	accountStatus->Text = FromUtf8(text.str());
	accountStatus->Opacity = 1.0;
	accountStatusIcon->Opacity = 1.0;
}

void DirectXPage::TryConfigureDefaultGamepad()
{
	if (!m_main || !m_cemuReady || m_gameRunning)
		return;

	// Do not schedule this through a PPL worker.  The Xbox shell can deliver
	// Gamepad events while that worker races the Cemu input/update threads.
	// This call only consumes the already-published POD state and is made on
	// the XAML thread, before a title is allowed to run.
	PublishGamepadStates();
	m_gamepadProfileReady = m_main->EnsureDefaultGamepadProfile();
	UpdateGamepadStatus();
}

void DirectXPage::OnBrokeredProgress(uint64_t bytesCopied, uint64_t totalBytes, const std::string& path)
{
	Platform::WeakReference weakThis(this);
	if (totalBytes == 0)
	{
		const auto detail = path.empty() ? "Indexing external title folders..." : path;
		auto text = FromUtf8(detail);
		create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
			ref new DispatchedHandler([weakThis, text]()
			{
				auto page = weakThis.Resolve<DirectXPage>();
				if (!page) return;
				page->launchStatus->Text = text;
				if (!page->m_externalLoadingVisible)
					return;
				page->externalLoadingTitle->Text = "Preparing external game";
				page->externalLoadingDetail->Text = text;
				page->externalLoadingProgress->IsIndeterminate = true;
				page->externalLoadingPercent->Text = "...";
			})));
		return;
	}
	if (path == "External title mounted")
	{
		auto text = FromUtf8(path);
		create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
			ref new DispatchedHandler([weakThis, text]()
			{
				auto page = weakThis.Resolve<DirectXPage>();
				if (!page) return;
				page->launchStatus->Text = text;
				if (!page->m_externalLoadingVisible)
					return;
				page->externalLoadingProgress->IsIndeterminate = false;
				page->externalLoadingProgress->Value = 100.0;
				page->externalLoadingPercent->Text = "100%";
				page->externalLoadingDetail->Text = text;
				// The brokered-storage phase is complete. Remove the XAML overlay now
				// so Cemu's native shader-cache progress, rendered into the underlying
				// SwapChainPanel, remains visible while title startup continues.
				page->SetExternalLoadingVisible(false);
			})));
		return;
	}
	std::ostringstream status;
	const double copiedGiB = static_cast<double>(bytesCopied) / (1024.0 * 1024.0 * 1024.0);
	const double totalGiB = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
	const double percent = totalBytes ? (100.0 * static_cast<double>(bytesCopied) / static_cast<double>(totalBytes)) : 0.0;
	const double displayPercent = (std::max)(0.0, (std::min)(100.0, percent));
	status << "Loading title: " << std::fixed << std::setprecision(1) << percent
		<< "% (" << std::setprecision(2) << copiedGiB << " / " << totalGiB << " GiB)";
	auto text = FromUtf8(status.str());
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([weakThis, text, displayPercent]()
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->launchStatus->Text = text;
			if (!page->m_externalLoadingVisible)
				return;
			page->externalLoadingProgress->IsIndeterminate = false;
			page->externalLoadingProgress->Value = displayPercent;
			page->externalLoadingPercent->Text = FromUtf8(
				std::to_string(static_cast<unsigned int>(std::lround(displayPercent))) + "%");
			page->externalLoadingDetail->Text = text;
		})));
}

void DirectXPage::UpdateStartButton()
{
	const int selectedIndex = FindInstalledTitleIndex(m_selectedTitleId);
	const bool canStart = m_main != nullptr && m_cemuReady && !m_libraryBusy &&
		selectedIndex >= 0;
	startButton->IsEnabled = canStart;
	startButton->Visibility = canStart ? VisibleValue : CollapsedValue;
	selectedGameStatus->Text = selectedIndex >= 0
		? FromUtf8(m_installedTitles[static_cast<size_t>(selectedIndex)].name)
		: ref new Platform::String(L"Select a game");
	UpdateSelectedShaderCount();
}

void DirectXPage::UpdateSelectedShaderCount()
{
	uint32_t shaderCount = 0;
	const int selectedIndex = FindInstalledTitleIndex(m_selectedTitleId);
	if (selectedIndex >= 0 && m_main && m_cemuReady)
	{
		const auto& title = m_installedTitles[static_cast<size_t>(selectedIndex)];
		const uint64_t cacheTitleId = title.graphicPackTitleId != 0
			? title.graphicPackTitleId : title.titleId;
		m_main->GetShaderCount(cacheTitleId, &shaderCount);
	}
	shaderCacheStatus->Text = FromUtf8("Shader cache     " +
		std::to_string(shaderCount) + (shaderCount == 1 ? " shader" : " shaders"));
}

int DirectXPage::FindInstalledTitleIndex(uint64_t titleId) const
{
	if (titleId == 0)
		return -1;
	for (size_t index = 0; index < m_installedTitles.size(); ++index)
		if (m_installedTitles[index].titleId == titleId)
			return static_cast<int>(index);
	return -1;
}

void DirectXPage::SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state)
{
	(void)state;
}

void DirectXPage::LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state)
{
	(void)state;
}
