# ---------------------------
# Variables
# ---------------------------
.PHONY: build configure test clean coverage dev-up dev-down build-main run-main

IMAGE_NAME = edavki-dev
BUILD_DIR = build
CACHE_VOLUME = $(IMAGE_NAME)-cache
CONTAINER_NAME = edavki-container
TEST_FILES := test_report_loader test_xml_generator test_application_service test_gui

# Detect environment: skip docker exec if already inside the container
INSIDE_DOCKER := $(shell if [ -f /.dockerenv ]; then echo "yes"; else echo "no"; fi)

ifeq ($(INSIDE_DOCKER),yes)
    CMD_PREFIX := 
else
    CMD_PREFIX := docker exec $(CONTAINER_NAME)
endif

# ---------------------------
# Image & Environment (Host Only)
# ---------------------------

build-image:
	@echo "Building development image..."
	@docker build --target dev-base -t $(IMAGE_NAME) .

dev-up:
	@if [ ! $$(docker images -q $(IMAGE_NAME)) ]; then $(MAKE) build-image; fi
	@if [ $$(docker ps -aq -f name=$(CONTAINER_NAME)) ]; then \
		docker start $(CONTAINER_NAME) > /dev/null; \
	else \
		docker run -d --name $(CONTAINER_NAME) \
			-v "$(PWD)":/app -v $(CACHE_VOLUME):/app/$(BUILD_DIR) \
			-e DISPLAY=$(DISPLAY) -v /tmp/.X11-unix:/tmp/.X11-unix \
			--user $(shell id -u):$(shell id -g) $(IMAGE_NAME) sleep infinity; \
	fi
	@xhost +local:docker > /dev/null
	@echo "Development engine is READY."

dev-down:
	@echo "Shutting down the development engine..."
	@docker stop $(CONTAINER_NAME) > /dev/null 2>&1 || true
	@docker rm $(CONTAINER_NAME) > /dev/null 2>&1 || true
	@echo "Engine STOPPED."

# ---------------------------
# Development Commands (Work both on Host and Inside Container)
# ---------------------------

configure:
	$(CMD_PREFIX) cmake -G Ninja -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug

build-test-report-loader:
	$(CMD_PREFIX) cmake --build $(BUILD_DIR) --target test_report_loader -j$(shell nproc)

build-test-xml-generator:
	$(CMD_PREFIX) cmake --build $(BUILD_DIR) --target test_xml_generator -j$(shell nproc)

build-test-application-service:
	$(CMD_PREFIX) cmake --build $(BUILD_DIR) --target test_application_service -j$(shell nproc)

build-test-gui:
	$(CMD_PREFIX) cmake --build $(BUILD_DIR) --target test_gui -j$(shell nproc)

# Build only the main application binary
build-main:
	$(CMD_PREFIX) cmake --build $(BUILD_DIR) --target EdavkiXmlMaker -j$(shell nproc)

build:
	$(CMD_PREFIX) cmake --build $(BUILD_DIR) -j$(shell nproc)

run: build
	$(CMD_PREFIX) ./$(BUILD_DIR)/EdavkiXmlMaker

# Launch the GUI application on your host display
run-main: build-main
	$(CMD_PREFIX) ./$(BUILD_DIR)/EdavkiXmlMaker

test: build
	$(CMD_PREFIX) bash -c "QT_QPA_PLATFORM=offscreen ctest --test-dir $(BUILD_DIR) --output-on-failure"

coverage: build
	@echo "Generating code coverage report..."
	# 1. Reset counters
	$(CMD_PREFIX) lcov --directory build --zerocounters
	
	# 2. Run all tests (ignore SegFault with '-')
	-$(CMD_PREFIX) bash -c "QT_QPA_PLATFORM=offscreen ctest --test-dir build"
	
	# 3. Capture coverage data
	$(CMD_PREFIX) lcov --directory build --capture --output-file build/coverage.info
	
	# 4. FILTER: Keep only src subfolders and the root util folder
	$(CMD_PREFIX) lcov --extract build/coverage.info "*/src/app/*" "*/src/backend/*" "*/src/gui/*" "*/src/util/*" --output-file build/coverage_filtered.info
	
	# 5. Generate HTML report and strip path prefixes for a clean view
	# Using --prefix /app/src and --prefix /app ensures all folders look like top-level names
	$(CMD_PREFIX) genhtml build/coverage_filtered.info \
		--prefix /app/src \
		--prefix /app \
		--output-directory tests/coverage_report
	@echo "Report generated at tests/coverage_report/index.html"

clean:
	$(CMD_PREFIX) rm -rf $(BUILD_DIR)/*

# Hard clean
# Use this when clean does not work
# It forces: Empty Folder -> New Config -> Fresh Build
clean-rebuild: clean configure build



# ---------------------------
# Production
# ---------------------------
VERSION ?= 1.0.0
LINUX_VERSION_DIR = production/linux/v$(VERSION)/bin

# AppImage
release-linux:
	@echo "Manufacturing Linux Standalone (v$(VERSION))..."
	@mkdir -p $(LINUX_VERSION_DIR)
	@chmod +x production/linux/bundle.sh
	
	# Build the tool environment
	docker build --target bundler -t edavki-bundler .
	
	# Run the factory: Build THEN Bundle
	docker run --rm \
		-v "$(CURDIR)/$(LINUX_VERSION_DIR)":/export \
		-v "$(CURDIR)":/app \
		-e QT_QPA_PLATFORM=offscreen \
		edavki-bundler \
		bash -c "cmake -G Ninja -S /app -B /app/build_prod -DCMAKE_BUILD_TYPE=Release && \
			 cmake --build /app/build_prod --parallel $$(nproc) && \
			 /app/production/linux/bundle.sh $(VERSION)"