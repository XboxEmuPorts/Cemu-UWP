#pragma once

#include <initializer_list>

namespace Cemu_UWP_Host
{
	namespace Localization
	{
		Platform::String^ Get(Platform::String^ source);
		Platform::String^ GetLiteral(const wchar_t* source);
		Platform::String^ Format(
			const wchar_t* source,
			std::initializer_list<Platform::String^> arguments);
		void Attach(Windows::UI::Xaml::DependencyObject^ root);
	}
}
