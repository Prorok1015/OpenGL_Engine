#!/bin/bash

# Claude Framework Initialization Script
# This script automatically sets up the framework in your project

set -e  # Exit on error

echo "=========================================="
echo "  Claude Framework - Auto Initialization"
echo "=========================================="
echo ""

# Check if we're in a project directory
if [ ! -d ".git" ]; then
    echo "⚠️  Warning: Not in a git repository"
    echo "   (This is optional, but recommended)"
    echo ""
fi

# Get framework directory
FRAMEWORK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(pwd)"

echo "📍 Project directory: $PROJECT_DIR"
echo "📍 Framework directory: $FRAMEWORK_DIR"
echo ""

# Step 1: Create directory structure
echo "Step 1: Creating directory structure..."
mkdir -p docs/agent/learning
mkdir -p docs/human
echo "✅ Created docs/agent/learning and docs/human"
echo ""

# Step 2: Copy self-learning guides
echo "Step 2: Copying self-learning guides..."
cp -v "$FRAMEWORK_DIR/docs-template/agent/learning"/* docs/agent/learning/
echo "✅ Copied 7 self-learning files"
echo ""

# Step 3: Copy CLAUDE.md and MEMORY_RULES.md templates
echo "Step 3: Creating CLAUDE.md and MEMORY_RULES.md..."
if [ -f "CLAUDE.md" ]; then
    echo "⚠️  CLAUDE.md already exists, skipping..."
else
    cp "$FRAMEWORK_DIR/CLAUDE_TEMPLATE.md" CLAUDE.md
    echo "✅ Created CLAUDE.md from template"
fi

if [ -f "MEMORY_RULES.md" ]; then
    echo "⚠️  MEMORY_RULES.md already exists, skipping..."
else
    cp "$FRAMEWORK_DIR/MEMORY_RULES_TEMPLATE.md" MEMORY_RULES.md
    echo "✅ Created MEMORY_RULES.md from template"
fi
echo "   ⚡ Edit both files now: Update [PROJECT_NAME] and customize rules"
echo ""

# Step 4: Create .claudesignore
echo "Step 4: Creating .claudesignore..."
if [ -f ".claudesignore" ]; then
    echo "⚠️  .claudesignore already exists, skipping..."
else
    cat > .claudesignore << 'EOF'
# Build artifacts
build/
dist/
out/
node_modules/
__pycache__/
venv/

# IDE caches
.vs/
.vscode/
.idea/

# Keep human docs separate
docs/human/

# Generated
*.log
*.tmp
EOF
    echo "✅ Created .claudesignore"
fi
echo ""

# Step 5: Create basic TOON files
echo "Step 5: Creating basic TOON files..."

# README.toon
cat > docs/agent/README.toon << 'EOF'
[DOCUMENTATION_INDEX]
format: TOON
audience: Claude agents

[FILES]
- PROJECT.toon → Project overview
- ARCHITECTURE.toon → System design
- CONSTRAINTS.toon → Rules and limits
- QUICK_LINKS.toon → File navigation

[USAGE]
Before working:
1. Read PROJECT.toon
2. Check CONSTRAINTS.toon (STRICT!)
3. Use QUICK_LINKS.toon

[LEARNING]
Update documentation:
→ See learning/SELF-LEARNING.toon
EOF
echo "✅ Created docs/agent/README.toon"

# PROJECT.toon
cat > docs/agent/PROJECT.toon << 'EOF'
[PROJECT]
name: [YOUR_PROJECT]
type: [Type]
description: [Description]

[MODULES]
[MODULE.1]
name: [Component]
responsibility: [What it does]

[STATUS]
current: [Current phase]
next: [Next steps]
EOF
echo "✅ Created docs/agent/PROJECT.toon"

# ARCHITECTURE.toon
cat > docs/agent/ARCHITECTURE.toon << 'EOF'
[ARCHITECTURE]
pattern: [Your pattern]

[COMPONENT_1]
name: [Name]
responsibility: [What it does]

[CONSTRAINTS]
rule: [Important rule 1]
rule: [Important rule 2]
EOF
echo "✅ Created docs/agent/ARCHITECTURE.toon"

# CONSTRAINTS.toon
cat > docs/agent/CONSTRAINTS.toon << 'EOF'
[CONSTRAINTS]
severity: CRITICAL

[CONSTRAINT_1]
rule: [Your first rule]
why: [Why it matters]

[CONSTRAINT_2]
rule: [Your second rule]
why: [Why it matters]
EOF
echo "✅ Created docs/agent/CONSTRAINTS.toon"

# QUICK_LINKS.toon
cat > docs/agent/QUICK_LINKS.toon << 'EOF'
[QUICK_LINKS]
purpose: Fast file lookup

[CODE]
Component 1 → [path]
Component 2 → [path]

[DOCUMENTATION]
Overview → PROJECT.toon
Design → ARCHITECTURE.toon
Rules → CONSTRAINTS.toon
EOF
echo "✅ Created docs/agent/QUICK_LINKS.toon"
echo ""

# Step 6: Summary
echo "=========================================="
echo "  ✅ Framework initialized!"
echo "=========================================="
echo ""
echo "Next steps:"
echo "1. Edit CLAUDE.md (replace [PROJECT_NAME] and update rules)"
echo "2. Edit docs/agent/PROJECT.toon (your project overview)"
echo "3. Edit docs/agent/ARCHITECTURE.toon (system design)"
echo "4. Edit docs/agent/CONSTRAINTS.toon (your rules)"
echo "5. Add to git:"
echo ""
echo "   git add CLAUDE.md .claudesignore docs/"
echo "   git commit -m 'docs: initialize agent documentation'"
echo ""
echo "6. Optionally remove claude-framework folder:"
echo ""
echo "   rm -rf claude-framework/"
echo ""
echo "📖 More info: Read INIT.md or README.md"
echo "🚀 Ready to work with Claude!"
echo ""
