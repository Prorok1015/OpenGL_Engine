#include "edt_project_trust_service.h"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
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
	if (m_flag) return true;
	if (!m_key) {
		egLOG_WARN("editor/trust", "module '{}' is not loaded: the project's path cannot be made canonical, so it "
			"cannot be trusted (pass --trust-project)", module);
		return false;
	}
	switch (judge_library(m_record, module, cmn::sha256_file_hex(copy))) {
	case library_verdict::load:
		return true;
	case library_verdict::refuse:
		egLOG_WARN("editor/trust", "module '{}' is not loaded: you did not trust this project (Project > Trust Project "
			"Modules... changes that)", module);
		return false;
	case library_verdict::ask:
		break;
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
	const std::optional<std::string> hash = cmn::sha256_file_hex(library);
	if (!hash) {
		egLOG_WARN("editor/trust", "the build of '{}' could not be read to remember it as the editor's own", module);
		return;
	}
	record_own_build_hash(module, *hash);
}

void edt::project_trust_service::record_own_build_hash(std::string_view module, std::string_view sha256)
{
	m_record.own_builds.insert_or_assign(std::string(module), std::string(sha256));
	if (!m_key) return;
	if (auto problem = store_own_build(m_res, *m_key, module, sha256)) {
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
	constexpr std::size_t k_trust = 0;
	dialogs.choose("Trust this project?", project_trust_question(service->waiting_modules(), service->current_answer()),
		{ "Trust", "Don't Trust" },
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
