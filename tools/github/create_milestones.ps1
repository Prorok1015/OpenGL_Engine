# Creates the tracker milestones in Prorok1015/OpenGL_Engine (skips ones that already exist)
# and prints their numbers. Requires GitHub CLI logged in with repo access: `gh auth login`.
# Run: powershell -ExecutionPolicy Bypass -File tools\github\create_milestones.ps1
param([string]$Repo = "Prorok1015/OpenGL_Engine")

$ErrorActionPreference = "Stop"
# gh receives arguments as UTF-8; make sure the console passes Cyrillic correctly.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$milestones = @(
	@("Фаза 0 — этап C", "Дешёвые фундаментальные решения: сервисы и DLL, форматы данных, явные фазы ECS, цикл кадра и player, платформенный шов, остаток EPIC-41"),
	@("Фаза 0 — этап D", "Средние редизайны: world_context, RHI шаги 1–4, правила шейдеров, POD-пакеты рендера, схема desc, task_system, каркас редактора"),
	@("M1 Проект и игровой модуль", "Система проектов, edt://, Game Module API, Editor Extension API, standalone player"),
	@("M2 Данные", "Схема и документы данных, идентичность ассетов (GUID), версии и миграции, кеш tag+type"),
	@("M3 Документный редактор", "Модель документов, Undo/Redo, Prefab Editor, autosave, picking, asset browser v2, материалы"),
	@("M4 Runtime", "Play mode, runtime-изображения и бюджет текстур, сервис сохранений"),
	@("M5 Игровой UI", "UI runtime, шрифты и текст, шаблоны и привязка данных, UI-редактор"),
	@("M6 Выпуск этапа А", "Cook & package, SDK и обновление проектов, диагностика и документация"),
	@("Этап Б (ТЗ)", "UI-анимации, звук, локализация")
)

$existing = gh api "repos/$Repo/milestones?state=all&per_page=100" --jq '.[].title'

foreach ($m in $milestones) {
	$title = $m[0]; $description = $m[1]
	if ($existing -contains $title) {
		Write-Host "exists:  $title"
	} else {
		gh api "repos/$Repo/milestones" -f "title=$title" -f "description=$description" --silent
		Write-Host "created: $title"
	}
}

Write-Host ""
Write-Host "Milestone numbers:"
gh api "repos/$Repo/milestones?state=all&per_page=100" --jq '.[] | "\(.number)\t\(.title)"'
