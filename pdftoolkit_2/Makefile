.PHONY: install install-all test coverage http-server clean

install:            ## Install the engine in editable mode with dev tools
	pip install -e ".[dev]"

install-all:        ## Install with every optional extra
	pip install -e ".[all]"

test:               ## Run the full test suite
	pytest

coverage:           ## Run tests with a coverage report
	pytest --cov=pdftoolkit --cov-report=term-missing

http-server:        ## Serve the HTTP adapter on http://localhost:8000
	pip install -e ".[http]" >/dev/null 2>&1
	uvicorn pdftoolkit.api.http:create_app --factory --host 0.0.0.0 --port 8000

clean:              ## Remove build and cache artefacts
	rm -rf build dist .pytest_cache .coverage htmlcov
	find . -type d -name "__pycache__" -exec rm -rf {} +
	find . -type d -name "*.egg-info" -exec rm -rf {} +
