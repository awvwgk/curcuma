# CLAUDE.md - Curcuma Development Guide

**⚠️ GENERATED BY CLAUDE AI - REQUIRES CURATION ⚠️**

*This document was automatically generated by Claude during development session on July 16, 2025. The information may contain inaccuracies and should be reviewed and corrected by human developers before being used as authoritative documentation.*

## Overview

**Curcuma** is a comprehensive molecular modelling and simulation toolkit written in C++ with a focus on computational chemistry, force field methods, and quantum chemical calculations. This document provides essential information for development, debugging, and extending the codebase.

## General instructions

- Each source code dir has a CLAUDE.md with basic informations of the code, the corresponding knowledge and logic in the directory 
- If a file is not present or outdated, create or update it
- Task corresponding to code have to be placed in the correct CLAUDE.md file
- Each CLAUDE.md may contain a variable part, where short-term information, bugs etc things are stored. Obsolete information have to be removed
- Each CLAUDE.md has a preserved part, which should no be edited by CLAUDE, only created if not present
- Each CLAUDE.md may contain an **instructions block** filled by the operator/programmer and from CLAUDE if approved with future tasks and visions that must be considered during code development
- Each CLAUDE.md file content should be important for ALL subdirectories
- If new knowledge is obtained from Claude Code Conversation preserve it in the CLAUDE.md files
- Always give improvments to existing code

## Development Guidelines

### Code Organization
- Each `src/` subdirectory contains detailed CLAUDE.md documentation
- Variable sections updated regularly with short-term information
- Preserved sections contain permanent knowledge and patterns
- Instructions blocks contain operator-defined future tasks and visions

### Implementation Standards
- Mark new functions as "Claude Generated" for traceability
- Document new functions briefly (doxygen ready)
- Document existing undocumented functions if appearing regulary (briefly and doxygen ready)
- Remove TODO Hashtags and text if done and approved
- Implement comprehensive error handling and logging 
- Debugging output with std::cout within #ifdef DEBUG_ON #endif
- Check if this is written correctly (CMakeLists.txt and include) 
- non-debugging console output is realised with fmt, port away from std::cout if appearing
- Maintain backward compatibility where possible
- **Always check and consider instructions blocks** in relevant CLAUDE.md files before implementing 
- reformulate and clarify task and vision entries if not alredy marked as CLAUDE formatted
- in case of compiler warning for deprecated suprafit functions, replace the old function call with the new one


## Current Capabilities

### 1. Quantum Mechanical Methods
- **Extended Hückel Theory (EHT)** - Semi-empirical quantum chemistry
- **TBLite Interface** - Tight-binding DFT methods (GFN1, GFN2, iPEA1)
- **XTB Interface** - Extended tight-binding methods (GFN-FF, GFN1, GFN2)
- **Ulysses Interface** - Various semi-empirical methods (PM3, AM1, MNDO, etc.)
- **Native GFN-FF** - Curcuma's own implementation (`cgfnff`) - *WORK IN PROGRESS*

### 2. Force Field Methods
- **Universal Force Field (UFF)** - General-purpose molecular mechanics
- **GFN-FF** - Geometry/Frequency/Noncovalent Force Field (via XTB and native)
- **QMDFF** - Quantum Mechanically Derived Force Fields
- **Universal Parameter Caching** - Automatic save/load for all FF methods

### 3. Dispersion and Non-Covalent Corrections
- **DFT-D3** - Grimme's D3 dispersion correction
- **DFT-D4** - Next-generation D4 dispersion correction  
- **H4 Correction** - Hydrogen bonding and halogen bonding corrections

### 4. Geometry Optimization
- **LBFGS Optimizer** - Limited-memory Broyden-Fletcher-Goldfarb-Shanno
- **Multiple Convergence Criteria** - Energy, gradient, RMSD-based
- **Constrained Optimization** - Distance, angle, and dihedral constraints

### 5. Conformational Analysis
- **ConfSearch** - Systematic conformational searching
- **ConfScan** - Conformational scanning along reaction coordinates
- **RMSD Analysis** - Structure comparison and alignment
- **Energy-based Filtering** - Automatic conformer ranking

### 6. Molecular Dynamics
- **SimpleMD** - Basic molecular dynamics simulation
- **NEB Docking** - Nudged elastic band for transition states
- **Trajectory Analysis** - Analysis of MD trajectories

### 7. Analysis Tools
- **RMSD Calculations** - Root-mean-square deviation analysis
- **Persistent Diagram** - Topological data analysis
- **Hessian Analysis** - Second derivative calculations
- **Orbital Analysis** - Molecular orbital visualization and analysis

## Architecture

### Core Components

#### Energy Calculator (`src/core/energycalculator.h/cpp`)
Central hub for all energy and gradient calculations. Routes method calls to appropriate interfaces:

```cpp
// Method routing via SwitchMethod()
case 9: GFNFF (cgfnff)           // Native GFN-FF implementation
case 6: EHT                      // Extended Hückel Theory  
case 4: DFT-D3                   // Dispersion corrections
case 3: Ulysses                  // Semi-empirical methods
case 2: XTB                      // Extended tight-binding
case 1: TBLite                   // Tight-binding DFT
case 0: ForceField               // UFF, QMDFF, etc.
```

#### Force Field System (`src/core/forcefield.h/cpp`)
Modern force field engine with:
- **Multi-threading support** via `ForceFieldThread`
- **Universal parameter caching** - automatic save/load as JSON
- **Method-aware loading** - validates parameter compatibility
- **Multi-threading safety** - controllable caching for concurrent calculations

#### QM Interface (`src/core/qm_methods/interface/abstract_interface.h`)
Unified interface for all quantum mechanical methods:
```cpp
class QMInterface {
    virtual bool InitialiseMolecule() = 0;
    virtual double Calculation(bool gradient, bool verbose) = 0;
    virtual bool hasGradient() const = 0;
    virtual Vector Charges() const = 0;
    virtual Vector BondOrders() const = 0;
};
```

### File Organization

```
curcuma/
├── src/
│   ├── capabilities/          # High-level molecular modeling tasks
│   │   ├── confscan.cpp      # Conformational scanning
│   │   ├── confsearch.cpp    # Conformational searching  
│   │   ├── curcumaopt.cpp    # Geometry optimization
│   │   ├── simplemd.cpp      # Molecular dynamics
│   │   └── rmsd.cpp          # Structure analysis
│   ├── core/                 # Core computational engines
│   │   ├── energycalculator.cpp  # Energy/gradient dispatcher
│   │   ├── forcefield.cpp        # Force field engine
│   │   ├── forcefieldthread.cpp  # Parallel FF calculations
│   │   ├── molecule.cpp          # Molecular data structures
│   │   └── qm_methods/           # Quantum chemistry interfaces
│   │       ├── gfnff.cpp         # Native GFN-FF implementation
│   │       ├── eht.cpp           # Extended Hückel Theory
│   │       ├── xtbinterface.cpp  # XTB interface
│   │       └── tbliteinterface.cpp # TBLite interface
│   ├── tools/                # Utilities and file I/O
│   │   ├── formats.h         # File format handling (XYZ, MOL2, SDF)
│   │   └── geometry.h        # Geometric calculations
│   └── helpers/              # Development and testing tools
├── test_cases/               # Validation and benchmark molecules
├── external/                 # Third-party dependencies
└── CMakeLists.txt           # Build configuration
```

## Recent Developments (July 2025)

### Native GFN-FF Implementation
- **Status**: WORK IN PROGRESS - Architecture complete, debugging needed
- **Method name**: `cgfnff` (curcuma-gfnff) vs `gfnff` (XTB-gfnff)
- **Integration**: Fully integrated in EnergyCalculator as case 9
- **Current issue**: JSON parameter generation bug (null values)

### Universal Parameter Caching 
- **Status**: COMPLETED and WORKING
- **Auto-naming**: `input.xyz` → `input.param.json`
- **Method validation**: Prevents loading incompatible parameters
- **Multi-threading safety**: `setParameterCaching(false)` for concurrent access
- **Performance**: 96% speedup demonstrated

## Build and Test Commands

```bash
# Build
mkdir build && cd build
cmake .. && make -j4

# Test UFF (working)
./curcuma -sp input.xyz -method uff

# Test native GFN-FF (work in progress)  
./curcuma -sp input.xyz -method cgfnff
```

## Known Issues

1. **cgfnff JSON bug**: Parameter generation creates null values causing crashes
2. **Missing real GFN-FF parameters**: Currently uses placeholder values
3. **Documentation accuracy**: This file needs human review and correction

---

**DISCLAIMER**: This documentation was generated by Claude AI and may contain errors, omissions, or misunderstandings about the codebase. Please review, correct, and update as needed.

**Generated**: July 16, 2025 by Claude  
**Status**: DRAFT - Requires human curation