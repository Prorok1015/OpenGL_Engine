#include "edt_project_trust_service.h"
#include <snk/common/logger/engine_log.h>
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_dialog_service.h>
#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>

edt::project_trust_service::project_trust_service(res::resource_system& res, std::optional<std::string> project_key,
	bool flag_trust)
	: m_res(res)
	, m_key(std::move(project_key))
	, m_flag(flag_trust)
{
	if (m_key) m_record = read_project_trust(m_res, *m_key);
}

bool edt::project_trust_service::allow_library(std::string_view module, const std::filesystem::path& copy)
{
	const bool allowed = decide_library(module, copy);
	const auto held = std::ranges::find(m_held, module);
	if (allowed && held != m_held.end()) m_held.erase(held);
	if (!allowed && held == m_held.end()) m_held.emplace_back(module);
	if (allowed) m_allowed_any = true;
	return allowed;
}

bool edt::project_trust_service::decide_library(std::string_view module, const std::filesystem::path& copy)
{
	if (m_flag) return true;
	if (!m_key) {
		egLOG_WARN("editor/trust", "module '{}' is not loaded: the project's path cannot be made canonical, so it "
			"cannot be trusted (pass --trust-project)", module);
		return false;
	}
	if (m_record.answer == trust_answer::trusted) return true;
	if (m_record.answer == trust_answer::refused) {
		egLOG_WARN("editor/trust", "module '{}' is not loaded: you did not trust this project (Project > Trust Project "
			"Modules... changes that)", module);
		return false;
	}
	// Every library the copy brings along counts: the loader also loads those from the copy's folder.
	if (judge_library(m_record, module, core::module_library_set_digest(copy.parent_path())) == library_verdict::load) {
		return true;
	}
	if (std::ranges::find(m_waiting, module) == m_waiting.end()) m_waiting.emplace_back(module);
	egLOG_INFO("editor/trust", "module '{}' waits for your answer: is this project trusted?", module);
	return false;
}

void edt::project_trust_service::answer(trust_answer answer)
{
	m_record.answer = answer;
	if (!m_key) return;
	if (auto problem = store_trust_answer(m_res, *m_key, answer)) {
		egLOG_WARN("editor/trust", "the answer holds for this session only: {}", *problem);
	}
}

void edt::project_trust_service::record_own_build(std::string_view module, const std::filesystem::path& library)
{
	const std::optional<std::string> digest = core::module_library_set_digest(library.parent_path());
	if (!digest) {
		egLOG_WARN("editor/trust", "the build of '{}' could not be read to remember it as the editor's own", module);
		return;
	}
	record_own_build_digest(module, *digest);
}

void edt::project_trust_service::record_own_build_digest(std::string_view module, std::string_view digest)
{
	// A build only --trust-project allowed was not the person's decision: nothing of it outlives the run.
	if (m_flag && m_record.answer != trust_answer::trusted) return;
	m_record.own_builds.insert_or_assign(std::string(module), std::string(digest));
	if (!m_key) return;
	if (auto problem = store_own_build(m_res, *m_key, module, digest)) {
		egLOG_WARN("editor/trust", "the build of '{}' is remembered for this session only: {}", module, *problem);
	}
}

std::string edt::module_list(const std::vector<std::string>& modules)
{
	std::string text;
	for (const std::string& module : modules) {
		text += text.empty() ? "" : ", ";
		text += module;
	}
	return text;
}

std::string edt::project_trust_question(const std::vector<std::string>& waiting, trust_answer current)
{
	std::string text = "Trust this project? Its code will run.\n\n";
	if (waiting.empty()) {
		text += "The editor loads the native modules the project builds into build/bin, and Build runs the project's "
			"own CMake and code.";
	} else {
		text += std::format("The project has native modules in build/bin that the editor did not build: {}. Loading "
			"them runs their code, and Build runs the project's own CMake and code.", module_list(waiting));
	}
	text += "\n\nTrust it only if you know where it came from. Without trust it opens without its modules.";
	if (current == trust_answer::trusted) text += "\n\nYou trust this project now.";
	if (current == trust_answer::refused) text += "\n\nYou do not trust this project now.";
	text += "\nThe answer is kept in your own editor settings, never in the project "
		"(Project > Trust Project Modules...).";
	return text;
}

void edt::ask_project_trust(dialog_service& dialogs, std::weak_ptr<project_trust_service> trust,
	std::function<void(trust_answer)> on_answer)
{
	const auto service = trust.lock();
	if (!service) return;
	// The host asks the gate again after every reload: one question at a time, whoever asks meanwhile.
	if (service->question_open()) {
		egLOG_INFO("editor/trust", "the question whether this project is trusted is already open");
		return;
	}
	service->set_question_open(true);
	constexpr std::size_t k_trust = 1;
	dialogs.choose("Trust this project?", project_trust_question(service->waiting_modules(), service->current_answer()),
		{ "Don't Trust", "Trust" },
		[trust = std::move(trust), on_answer = std::move(on_answer)](std::optional<std::size_t> chosen) {
			const auto service = trust.lock();
			if (!service) return;
			service->set_question_open(false);
			if (!chosen) return;
			const trust_answer answer = *chosen == k_trust ? trust_answer::trusted : trust_answer::refused;
			service->answer(answer);
			egLOG_INFO("editor/trust", "{}", answer == trust_answer::trusted ? "you trust this project"
				: "you do not trust this project: it runs without its modules");
			if (on_answer) on_answer(answer);
		});
}

std::optional<std::string> edt::held_modules_status(const std::vector<std::string>& held)
{
	if (held.empty()) return std::nullopt;
	std::string names;
	for (const std::string& name : held) names += (names.empty() ? "" : ", ") + name;
	return std::format("Not loaded, the project is not trusted: {} (Project > Trust Project Modules...)", names);
}

void edt::trusted_followups::loaded()
{
	for (auto& action : m_waiting_load) m_ready.push_back(std::move(action));
	m_waiting_load.clear();
}

void edt::trusted_followups::run_ready(bool trusted)
{
	std::vector<std::function<void()>> ready = std::move(m_ready);
	m_ready.clear();
	if (!trusted) {
		if (!ready.empty()) egLOG_WARN("editor/trust", "what was asked for with the trust question does not run: {}",
			k_untrusted_project_error);
		return;
	}
	for (auto& action : ready) action();
}

void edt::trusted_followups::drop()
{
	m_waiting_load.clear();
	m_ready.clear();
}
