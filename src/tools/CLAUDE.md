# CLAUDE.md - Tools Directory

## Overview

The tools directory contains utility functions and header-only libraries that provide essential services across the entire Curcuma codebase. These are fundamental building blocks used by all other modules.

## Structure

```
tools/
├── formats.h     # File format handling (XYZ, MOL2, SDF, PDB)
├── geometry.h    # Geometric calculations and transformations
├── general.h     # General utility functions and constants
└── info.h        # Information and metadata handling
```

## Key Components

### File Format Support (`formats.h`)
- **XYZ files**: Standard coordinate format reading/writing
- **MOL2 files**: Tripos molecular structure format
- **SDF files**: Structure-data file format
- **PDB files**: Protein Data Bank format
- **JSON files**: Parameter and configuration storage
- Automatic format detection and validation
- Error handling for malformed files

### Geometric Operations (`geometry.h`)
- **Distance calculations**: Bond lengths, inter-atomic distances
- **Angle calculations**: Bond angles, dihedral angles
- **Coordinate transformations**: Rotation, translation, alignment
- **Molecular alignment**: RMSD-based structure superposition
- **Center of mass**: Molecular center calculations
- **Moments of inertia**: Principal axis determination

### General Utilities (`general.h`)
- **Constants**: Physical constants, conversion factors
- **String operations**: Parsing, formatting utilities
- **Mathematical functions**: Vector operations, matrix utilities
- **Memory management**: Smart pointer utilities
- **Error handling**: Exception classes and error codes

### Information Handling (`info.h`)
- **Atomic data**: Element properties, atomic masses, radii
- **Periodic table**: Element lookup and properties
- **Units and conversions**: Energy, length, angle conversions
- **Metadata**: File headers, calculation information

## Design Principles

### Header-Only Implementation
- All tools are header-only for easy inclusion and compilation
- Template-based design for type flexibility
- Inline functions for performance optimization

### Cross-Platform Compatibility
- Platform-independent implementations
- Consistent behavior across different systems
- Standard library dependencies only

### Performance Optimization
- Efficient algorithms for geometric calculations
- Memory-efficient data structures
- Vectorized operations where possible

## Instructions Block

**PRESERVED - DO NOT EDIT BY CLAUDE**

*Tool development priorities and utility function requirements to be defined by operator/programmer*

## Variable Section

### Current Development
- Enhanced file format support for newer molecular formats
- Improved geometric calculation performance
- Better error handling and validation

### Recent Improvements
- Streamlined file I/O operations
- Enhanced coordinate transformation functions
- Better integration with molecular data structures

---

*This documentation covers all utility functions and tools used throughout Curcuma*