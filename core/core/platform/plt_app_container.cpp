// Its own translation unit (#1179): only the editor calls ensure(), so a static game.exe that links plt_process never pulls
// this object in, and with it no userenv.dll import.
#include <snk/platform/plt_process.h>
#include "plt_app_container_state.h"
#include <memory>
#include <utility>

#ifdef _WIN32
#include <filesystem>
#include <format>
#include <system_error>
#include <userenv.h>
#endif

#ifdef _WIN32

std::shared_ptr<const platform::app_container> platform::app_container::ensure(std::wstring name)
{
	auto held = std::make_unique<state>();
	held->name = std::move(name);
	if (held->name.empty()) {
		held->error = "the AppContainer has no name";
	} else {
		HRESULT made = CreateAppContainerProfile(held->name.c_str(), held->name.c_str(), L"snk model decoder (#1054)", nullptr, 0,
			&held->sid);
		if (made == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
			held->sid = nullptr;
			made = DeriveAppContainerSidFromAppContainerName(held->name.c_str(), &held->sid);
		}
		if (FAILED(made)) {
			held->sid = nullptr;
			const std::u8string name_text = std::filesystem::path(held->name).u8string();
			held->error = std::format("cannot make the AppContainer profile '{}': {}", std::string(name_text.begin(), name_text.end()),
				std::error_code{ static_cast<int>(HRESULT_CODE(made)), std::system_category() }.message());
		}
	}
	return std::shared_ptr<const app_container>(new app_container(std::move(held)));
}

#else

std::shared_ptr<const platform::app_container> platform::app_container::ensure(std::wstring name)
{
	auto held = std::make_unique<state>();
	held->name = std::move(name);
	held->error = "this system has no AppContainer";
	return std::shared_ptr<const app_container>(new app_container(std::move(held)));
}

#endif
