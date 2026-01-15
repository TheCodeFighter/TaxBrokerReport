# TaxBrokerReport – Architecture & Processing Pipeline

## Purpose

Cross-platform C++ application that converts broker PDF reports → structured JSON → official Slovenian tax XML (eDavki).

**Design goals:**

- clear separation of concerns
- reusable processing stages
- future Python bindings
- multiple frontend (GUI / CLI / batch)

---

## High-Level Pipeline

**PDF → JSON → XML**  
Each stage is isolated and independently reusable.

---

## Step 1: PDF → JSON (Parsing Layer)

**Responsibility**  
Convert broker-specific PDF reports into normalized, broker-independent JSON.

**Main component:** `ReportLoader`

**What it does:**

- extracts raw text from PDF
- parses various broker formats
- outputs stable JSON schema with financial transactions and metadata

**Key characteristics:**

- Pure data extraction & normalization
- Fully unit-tested
- Reusable as: library / Python binding / batch tool

---

## Step 2: JSON → XML (Domain & XML Layer)

**Responsibility**  
Transform normalized JSON into valid eDavki XML documents.

**Main component:** `XmlGenerator`

**What it does:**

- reads normalized JSON
- maps transactions to tax models (KDVP, DIV, DHO, ...)
- generates XML using official schemas

**Key characteristics:**

- Tax logic only
- No PDF knowledge
- Easily extensible for new forms

---

## Step 3: Application Layer (Orchestration)

**Responsibility**  
Coordinates pipeline and provides clean API.

**Main component:** `ApplicationService`

**What it does:**

- runs complete PDF→JSON→XML flow
- validates input & configuration
- hides implementation details

**Consumers:**

- Qt GUI
- CLI (planned)
- Python bindings (planned)
- Batch/automation

---

## Step 4: GUI Layer

**Technology:** Qt 6 (Widgets)

**Responsibility:**  
User interaction only

- File selection
- Parameter input
- Progress & results visualization

**Important:** GUI never touches parsing or XML generation directly.

---

## Logical Directory Structure

``` text
include/
├── backend/
│   ├── report_loader.hpp
│   └── xml_generator.hpp
├── app/
│   └── application_service.hpp
src/
├── backend/
│   ├── report_loader.cpp
│   └── xml_generator.cpp
├── app/
│   └── application_service.cpp
└── main.cpp
```

---

## Summary – Core Layers

- **PDF → JSON**     – data normalization, broker format handling  
- **JSON → XML**     – tax domain logic & official schema compliance  
- **Application**    – orchestration, validation, stable API  
- **GUI**            – presentation only (replaceable)

This strict layering gives excellent maintainability, testability and future extensibility.
