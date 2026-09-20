#include "pch.h"
#include "Localization.h"

#include <cwchar>
#include <string>
#include <vector>

using namespace Windows::ApplicationModel::Resources;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

namespace Cemu_UWP_Host
{
	namespace Localization
	{
		namespace
		{
			uint64_t Hash(Platform::String^ source)
			{
				uint64_t hash = 14695981039346656037ull;
				for (unsigned int index = 0; index < source->Length(); ++index)
				{
					hash ^= static_cast<uint16_t>(source->Data()[index]);
					hash *= 1099511628211ull;
				}
				return hash;
			}

			bool Same(Platform::String^ left, Platform::String^ right)
			{
				if (left == right)
					return true;
				if (!left || !right || left->Length() != right->Length())
					return false;
				return std::wmemcmp(left->Data(), right->Data(), left->Length()) == 0;
			}

			Platform::String^ TranslateObjectString(Platform::Object^ value)
			{
				auto text = dynamic_cast<Platform::String^>(value);
				return text ? Get(text) : nullptr;
			}

			void TranslateTextBlock(TextBlock^ control)
			{
				auto translated = Get(control->Text);
				if (!Same(translated, control->Text))
					control->Text = translated;
			}

			void TranslateContent(ContentControl^ control)
			{
				auto current = dynamic_cast<Platform::String^>(control->Content);
				if (!current)
					return;
				auto translated = Get(current);
				if (!Same(translated, current))
					control->Content = translated;
			}

			void TranslatePivotHeader(PivotItem^ control)
			{
				auto current = dynamic_cast<Platform::String^>(control->Header);
				if (!current)
					return;
				auto translated = Get(current);
				if (!Same(translated, current))
					control->Header = translated;
			}

			void TranslateComboBox(ComboBox^ control)
			{
				auto header = dynamic_cast<Platform::String^>(control->Header);
				if (header)
				{
					auto translated = Get(header);
					if (!Same(translated, header))
						control->Header = translated;
				}
				auto placeholder = control->PlaceholderText;
				if (placeholder)
				{
					auto translated = Get(placeholder);
					if (!Same(translated, placeholder))
						control->PlaceholderText = translated;
				}
			}

			void TranslateSlider(Slider^ control)
			{
				auto header = dynamic_cast<Platform::String^>(control->Header);
				if (!header)
					return;
				auto translated = Get(header);
				if (!Same(translated, header))
					control->Header = translated;
			}

			void TranslateToggleSwitch(ToggleSwitch^ control)
			{
				auto header = dynamic_cast<Platform::String^>(control->Header);
				if (!header)
					return;
				auto translated = Get(header);
				if (!Same(translated, header))
					control->Header = translated;
			}

			void TranslateToolTip(DependencyObject^ control)
			{
				auto current = dynamic_cast<Platform::String^>(ToolTipService::GetToolTip(control));
				if (!current)
					return;
				auto translated = Get(current);
				if (!Same(translated, current))
					ToolTipService::SetToolTip(control, translated);
			}
		}

		Platform::String^ Get(Platform::String^ source)
		{
			if (!source || source->IsEmpty())
				return source;

			auto lookup = [](Platform::String^ candidate) -> Platform::String^
			{
				wchar_t key[32]{};
				swprintf_s(key, L"Text_%016llX",
					static_cast<unsigned long long>(Hash(candidate)));
				auto translated = ResourceLoader::GetForCurrentView()->GetString(
					ref new Platform::String(key));
				return translated && !translated->IsEmpty() ? translated : nullptr;
			};

			try
			{
				if (auto translated = lookup(source))
					return translated;

				// Runtime status strings often differ only by counters. Normalize
				// decimal runs to {0}, {1}, ... so one .resw template covers all
				// counts without forcing every caller to format its own resource.
				std::wstring normalized;
				std::vector<std::wstring> arguments;
				const wchar_t* data = source->Data();
				for (unsigned int index = 0; index < source->Length();)
				{
					if (data[index] < L'0' || data[index] > L'9')
					{
						normalized.push_back(data[index++]);
						continue;
					}
					const unsigned int begin = index;
					while (index < source->Length() &&
						data[index] >= L'0' && data[index] <= L'9')
						++index;
					arguments.emplace_back(data + begin, data + index);
					normalized += L"{" + std::to_wstring(arguments.size() - 1) + L"}";
				}

				if (!arguments.empty())
				{
					auto templ = lookup(ref new Platform::String(normalized.c_str()));
					if (templ)
					{
						std::wstring result(templ->Data(), templ->Length());
						for (size_t index = 0; index < arguments.size(); ++index)
						{
							const std::wstring token =
								L"{" + std::to_wstring(index) + L"}";
							for (size_t position = 0;
								(position = result.find(token, position)) != std::wstring::npos;)
							{
								result.replace(position, token.length(), arguments[index]);
								position += arguments[index].length();
							}
						}
						return ref new Platform::String(result.c_str());
					}
				}
			}
			catch (...)
			{
			}
			return source;
		}

		Platform::String^ GetLiteral(const wchar_t* source)
		{
			return Get(ref new Platform::String(source));
		}

		Platform::String^ Format(
			const wchar_t* source,
			std::initializer_list<Platform::String^> arguments)
		{
			auto format = GetLiteral(source);
			std::wstring text(format->Data(), format->Length());
			size_t argumentIndex = 0;
			for (auto argument : arguments)
			{
				const std::wstring token = L"{" + std::to_wstring(argumentIndex++) + L"}";
				const std::wstring replacement =
					argument ? std::wstring(argument->Data(), argument->Length()) : std::wstring();
				for (size_t position = 0;
					(position = text.find(token, position)) != std::wstring::npos;)
				{
					text.replace(position, token.length(), replacement);
					position += replacement.length();
				}
			}
			return ref new Platform::String(text.c_str());
		}

		void Attach(DependencyObject^ root)
		{
			if (!root)
				return;

			if (auto text = dynamic_cast<TextBlock^>(root))
			{
				TranslateTextBlock(text);
				text->RegisterPropertyChangedCallback(
					TextBlock::TextProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslateTextBlock(safe_cast<TextBlock^>(sender));
						}));
			}

			if (auto content = dynamic_cast<ContentControl^>(root))
			{
				TranslateContent(content);
				content->RegisterPropertyChangedCallback(
					ContentControl::ContentProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslateContent(safe_cast<ContentControl^>(sender));
						}));
			}

			if (auto pivotItem = dynamic_cast<PivotItem^>(root))
			{
				TranslatePivotHeader(pivotItem);
				pivotItem->RegisterPropertyChangedCallback(
					PivotItem::HeaderProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslatePivotHeader(safe_cast<PivotItem^>(sender));
						}));
			}

			if (auto combo = dynamic_cast<ComboBox^>(root))
			{
				TranslateComboBox(combo);
				combo->RegisterPropertyChangedCallback(
					ComboBox::HeaderProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslateComboBox(safe_cast<ComboBox^>(sender));
						}));
				combo->RegisterPropertyChangedCallback(
					ComboBox::PlaceholderTextProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslateComboBox(safe_cast<ComboBox^>(sender));
						}));
			}

			if (auto slider = dynamic_cast<Slider^>(root))
			{
				TranslateSlider(slider);
				slider->RegisterPropertyChangedCallback(
					Slider::HeaderProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslateSlider(safe_cast<Slider^>(sender));
						}));
			}

			if (auto toggle = dynamic_cast<ToggleSwitch^>(root))
			{
				TranslateToggleSwitch(toggle);
				toggle->RegisterPropertyChangedCallback(
					ToggleSwitch::HeaderProperty,
					ref new DependencyPropertyChangedCallback(
						[](DependencyObject^ sender, DependencyProperty^)
						{
							TranslateToggleSwitch(safe_cast<ToggleSwitch^>(sender));
						}));
			}

			TranslateToolTip(root);

			if (auto userControl = dynamic_cast<UserControl^>(root))
				Attach(userControl->Content);

			if (auto panel = dynamic_cast<Panel^>(root))
			{
				for (unsigned int index = 0; index < panel->Children->Size; ++index)
					Attach(panel->Children->GetAt(index));
			}

			if (auto content = dynamic_cast<ContentControl^>(root))
			{
				auto child = dynamic_cast<DependencyObject^>(content->Content);
				if (child)
					Attach(child);
			}

			if (auto items = dynamic_cast<ItemsControl^>(root))
			{
				for (unsigned int index = 0; index < items->Items->Size; ++index)
				{
					auto child = dynamic_cast<DependencyObject^>(items->Items->GetAt(index));
					if (child)
						Attach(child);
				}
			}

			if (auto border = dynamic_cast<Border^>(root))
				Attach(border->Child);
		}
	}
}
