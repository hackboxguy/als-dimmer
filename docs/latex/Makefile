# Makefile for ALS-Dimmer Technical Documentation
# Author: Albert David
# Date: November 2025

# LaTeX compiler
LATEX = pdflatex
BIBTEX = bibtex

# Source and output directories
SRC_DIR = src
OUTPUT_DIR = output

# Main document
MAIN = als-dimmer-spec
SRC = $(SRC_DIR)/$(MAIN).tex
PDF = $(OUTPUT_DIR)/$(MAIN).pdf

# Build flags
LATEX_FLAGS = -output-directory=$(OUTPUT_DIR) -halt-on-error

.PHONY: all clean help

# Default target
all: $(PDF)

# Build PDF (run twice for cross-references and TOC)
$(PDF): $(SRC) $(wildcard $(SRC_DIR)/*.tex)
	@echo "=== Building ALS-Dimmer Technical Documentation ==="
	@mkdir -p $(OUTPUT_DIR)
	cd $(SRC_DIR) && $(LATEX) -output-directory=../$(OUTPUT_DIR) -halt-on-error $(MAIN).tex
	cd $(SRC_DIR) && $(LATEX) -output-directory=../$(OUTPUT_DIR) -halt-on-error $(MAIN).tex
	@echo "=== PDF generated: $(PDF) ==="

# Clean build artifacts (remove entire output directory)
clean:
	@echo "=== Cleaning build artifacts ==="
	rm -rf $(OUTPUT_DIR)
	@echo "=== Clean complete (output directory removed) ==="

# Help target
help:
	@echo "ALS-Dimmer Documentation Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all     - Build PDF (default)"
	@echo "  clean   - Remove build artifacts"
	@echo "  help    - Show this help message"
	@echo ""
	@echo "Requirements:"
	@echo "  - pdflatex (texlive-latex-base)"
	@echo "  - TikZ (texlive-pictures)"
	@echo "  - Additional packages (texlive-latex-extra)"
	@echo ""
	@echo "Build PDF:"
	@echo "  make"
	@echo ""
	@echo "Output: $(PDF)"
