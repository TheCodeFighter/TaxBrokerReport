# ---------------------------
# Variables
# ---------------------------
# Dev image with all tools/deps
IMAGE_NAME = edavki-dev

# Minimal runtime image
PROD_IMAGE = edavki-prod

# CMake build folder
BUILD_DIR = build

# Persistent build volume
CACHE_VOLUME = $(IMAGE_NAME)-cache

# For debugging/VSCode attach
CONTAINER_NAME = edavki-container
CONTAINER_NAME_NO_DEBUG = edavki-container-no-debug

VALGRIND_TESTS_FILES := test_report_loader test_xml_generator   # Update when new files are needed to ckeck (i.e. main)


# Common docker run command for dev tasks (mounts source + build cache)
DOCKER_RUN = docker run --rm --name $(CONTAINER_NAME_NO_DEBUG) \
	-v "$(PWD)":/app \
	-v $(CACHE_VOLUME):/app/$(BUILD_DIR) \
	--user $(shell id -u):$(shell id -g) \
	$(IMAGE_NAME)

# ---------------------------
# Targets
# ---------------------------

# 1. Build base dev image (installs deps, cached)
build-image:
	docker build --target dev-base -t $(IMAGE_NAME) .

# 2. Configure CMake project inside container
configure:
	docker run --rm --name $(CONTAINER_NAME_NO_DEBUG) \
	-v "$(PWD)":/app \
	-v $(CACHE_VOLUME):/app/$(BUILD_DIR) \
	--user $(shell id -u):$(shell id -g) \
	$(IMAGE_NAME) \
	cmake -S . -B /app/$(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug

# 3. Build incrementally inside container (uses persistent build cache)
build:
	$(DOCKER_RUN) cmake --build $(BUILD_DIR)

# 4. Run tests inside container
test: build
	$(DOCKER_RUN) cmake --build $(BUILD_DIR) --target test_report_loader test_xml_generator
	$(DOCKER_RUN) ctest --test-dir $(BUILD_DIR) --output-on-failure

# 5. Combined: configure + build + test
all: configure build test

# 6. Build production image (multi-stage, minimal size)
build-prod:
	docker build --target prod -t $(PROD_IMAGE) .

# 7. Run production image (executes built binary)
run-prod:
	docker run --rm $(PROD_IMAGE)

# 8. Optional: start interactive dev container (for debugging or manual work)
run-dev:
	docker run -d --name $(CONTAINER_NAME) -v "$(PWD)":/app -v $(CACHE_VOLUME):/app/$(BUILD_DIR) --user $(shell id -u):$(shell id -g) $(IMAGE_NAME) sleep infinity

# 9. Open shell inside dev container
dev-shell:
	docker run -it --rm \
	  --name $(CONTAINER_NAME) \
	  -v "$(PWD)":/app \
	  -v $(CACHE_VOLUME):/app/$(BUILD_DIR) \
	  --user $(shell id -u):$(shell id -g) \
	  $(IMAGE_NAME) /bin/bash

# -----------------------------
# 10. Debug tests in container
# -----------------------------
debug-test-report-loader:
	docker run --rm -it --name $(CONTAINER_NAME) \
	  -v "${PWD}":/app \
	  --user $(shell id -u):$(shell id -g) \
	  $(IMAGE_NAME) \
	  bash -c "cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && gdb ./build/test_report_loader"

# 11.a Start dev container and build tests
# ---------------------------
dev-container-build:
	@echo "Starting edavki-dev container and building tests..."
	@if [ $$(docker ps -aq -f name=$(CONTAINER_NAME)) ]; then \
		echo "Container already exists. Starting it..."; \
		docker start $(CONTAINER_NAME) > /dev/null; \
	else \
		docker run -d \
		  --name $(CONTAINER_NAME) \
		  -v "$(PWD)":/app \
		  -v $(CACHE_VOLUME):/app/$(BUILD_DIR) \
		  --user $(shell id -u):$(shell id -g) \
		  edavki-dev \
		  sleep infinity; \
	fi
	@echo "Running CMake build inside container..."
	docker exec $(CONTAINER_NAME) cmake -S /app -B /app/$(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	docker exec $(CONTAINER_NAME) cmake --build /app/$(BUILD_DIR)


# ---------------------------
# 11.b Optional: stop container
# ---------------------------
dev-container-stop:
	@echo "Stopping edavki-dev container..."
	docker stop $(CONTAINER_NAME) || echo "Container is not running"


# Build single test inside container, will not work outside
build-test-report-loader:
	@echo "Building test_report_loader..."
	cmake -S /app -B /app/$(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build /app/$(BUILD_DIR) --target test_report_loader

build-test-xml-generator:
	@echo "Building test_xml_generator..."
	cmake -S /app -B /app/$(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build /app/$(BUILD_DIR) --target test_xml_generator

# ---------------------------
# 12. Run all tests under Valgrind (inside container)
# ---------------------------
valgrind-tests: dev-container-build
	@echo "Running selected tests under Valgrind inside container..."
	docker exec -it $(CONTAINER_NAME) bash -c "\
	    for test_name in $(VALGRIND_TESTS_FILES); do \
	        test_exec=\"/app/$(BUILD_DIR)/\$$test_name\"; \
	        if [ -x \"\$$test_exec\" ]; then \
	            echo '==> Running Valgrind on' \$$test_exec; \
	            valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \$$test_exec && \
	            echo '✔ Passed Valgrind:' \$$test_exec; \
	        else \
	            echo 'Test executable not found or not executable:' \$$test_exec; \
	        fi; \
	    done"   
