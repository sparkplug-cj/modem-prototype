# Description: 
#     Configure compile option for individual sub targets.
#     Default option : -Wall : this option includes below option.
#         -Waddress
#         -Warray-bounds=1 (only with ‘-O2’)
#         -Warray-parameter=2 (C and Objective-C only)
#         -Wbool-compare
#         -Wbool-operation
#         -Wc++11-compat -Wc++14-compat
#         -Wcatch-value (C++ and Objective-C++ only)
#         -Wchar-subscripts
#         -Wcomment
#         -Wduplicate-decl-specifier (C and Objective-C only)
#         -Wenum-compare (in C/ObjC; this is on by default in C++)
#         -Wformat
#         -Wformat-overflow
#         -Wformat-truncation
#         -Wint-in-bool-context
#         -Wimplicit (C and Objective-C only)
#         -Wimplicit-int (C and Objective-C only)
#         -Wimplicit-function-declaration (C and Objective-C only)
#         -Winit-self (only for C++)
#         -Wlogical-not-parentheses
#         -Wmain (only for C/ObjC and unless ‘-ffreestanding’)
#         -Wmaybe-uninitialized
#         -Wmemset-elt-size
#         -Wmemset-transposed-args
#         -Wmisleading-indentation (only for C/C++)
#         -Wmissing-attributes
#         -Wmissing-braces (only for C/ObjC)
#         -Wmultistatement-macros
#         -Wnarrowing (only for C++)
#         -Wnonnull
#         -Wnonnull-compare
#         -Wopenmp-simd
#         -Wparentheses
#         -Wpessimizing-move (only for C++)
#         -Wpointer-sign
#         -Wrange-loop-construct (only for C++)
#         -Wreorder
#         -Wrestrict
#         -Wreturn-type
#         -Wsequence-point
#         -Wsign-compare (only in C++)
#         -Wsizeof-array-div
#         -Wsizeof-pointer-div
#         -Wsizeof-pointer-memaccess
#         -Wstrict-aliasing
#         -Wstrict-overflow=1
#         -Wswitch
#         -Wtautological-compare
#         -Wtrigraphs
#         -Wuninitialized
#         -Wunknown-pragmas
#         -Wunused-function
#         -Wunused-label
#         -Wunused-value
#         -Wunused-variable
#         -Wvla-parameter (C and Objective-C only)
#         -Wvolatile-register-var
#         -Wzero-length-bounds
#
#     -Wextra : includes belows
#         -Wclobbered
#         -Wcast-function-type
#         -Wdeprecated-copy (C++ only)
#         -Wempty-body
#         -Wenum-conversion (C only)
#         -Wignored-qualifiers
#         -Wimplicit-fallthrough=3
#         -Wmissing-field-initializers
#         -Wmissing-parameter-type (C only)
#         -Wold-style-declaration (C only)
#         -Woverride-init
#         -Wsign-compare (C only)
#         -Wstring-compare
#         -Wredundant-move (only for C++)
#         -Wtype-limits
#         -Wuninitialized
#         -Wshift-negative-value (in C++11 to C++17 and in C99 and newer)
#         -Wunused-parameter (only with ‘-Wunused’ or ‘-Wall’)
#         -Wunused-but-set-parameter (only with ‘-Wunused’ or ‘-Wall’)
#         - A pointer is compared against integer zero with <=, >, or >=
#
#      -Wpedantic       --> issue warning for non-standard code
#      -Wfatal-errors   --> stop compile at the first error.
#      -Werror          --> treat all warning as an error.

#      Other options
#     	  -Wcast-align=strict
#         -Wcast-function-type
#         -Wconversion   --> This will cover -Wsign-conversion and -Wfloat-conversion
#         -Wdangling-else 
#         -Wlogical-op
#         -Wno-aggressive-loop-optimizations
#         -Wstrict-prototypes
#         -Wold-style-definition
#         -Wmissing-prototypes (
#         -Wmissing-declarations
#         -Wpadded   ---> useful when defining structure - need to adjust the order of fields
#         -Wredundant-decls
#         -Wnested-externs
#         -Winline   --> if inline is not inlined
#         -Wlong-long   --> this is enabled by -Wpedantic… if we need to use long long, -Wno-long-long
#         -Wvariadic-macros  --> enabled by -Wpedantic… if needed, -Wno-variadic-macros
#         -Wvla   --> enabled by -Wpedantic. If needed -Wno-vla
#         -Wpointer-sign   --> enabled by -Wpedantic.   -Wno-pointer-sign      
#         -Wstack-protector  --> when -fstack-protector is active   ---TBD
#         -Wunsuffixed-float-constants   
#
#  NOTE : -Wpedantic will detect huge warning, so need to open the option for individual target.
#         -Werror will issue an error for all warning. this also need to be applied to individual target.
#==============================================================================================================    
#      TBD: This option needs further discussion. it will generate meaningful analysis.
#      Static Analysis option :
#         -fanalyzer    : this will includes
#               -Wanalyzer-double-fclose
#               -Wanalyzer-double-free
#               -Wanalyzer-exposure-through-output-file
#               -Wanalyzer-file-leak
#               -Wanalyzer-free-of-non-heap
#               -Wanalyzer-malloc-leak
#               -Wanalyzer-mismatching-deallocation
#               -Wanalyzer-possible-null-argument
#               -Wanalyzer-possible-null-dereference
#               -Wanalyzer-null-argument
#               -Wanalyzer-null-dereference
#               -Wanalyzer-shift-count-negative
#               -Wanalyzer-shift-count-overflow
#               -Wanalyzer-stale-setjmp-buffer
#               -Wanalyzer-tainted-array-index
#               -Wanalyzer-unsafe-call-within-signal-handler
#               -Wanalyzer-use-after-free
#               -Wanalyzer-use-of-pointer-in-stale-stack-frame
#               -Wanalyzer-write-to-const
#               -Wanalyzer-write-to-string-literal
#          -Wno-analyzer-XXXXX  --> for excluding specific option.     


# The following options are applied when building control firmware. This includes imported code, but excludes
# thirdparty / SOUP code.
set(COMPILE_OPTIONS_COMMON 
    # Base warning options
    -Wall
    -Wextra
    -Wpedantic               # Detect when going outside of ISO C standard
    -Werror                  # Treat all warnings as errors to avoid "broken windows" in firmware code.

    # Additional useful warnings that prevents unpredicted behaviour or bugs. Taken from SleepStyle, with some
    # options removed due to being covered by -Wall & -Wextra.
    -Wdisabled-optimization  # Warn if a requested optimization pass is disabled. May just be caused by overly complex code meaning gcc refuses to optimise
    -Wdouble-promotion       # Give a warning when a value of type float is implicitly promoted to double
    -Wfloat-equal            # Warn if floating-point values are used in equality comparisons
    -Wformat=2               # Warn about incorrect types supplied as args for printf etc.
    -Winline                 # Warn if a function that is declared as inline cannot be inlined by the compiler
    -Winvalid-pch            # Warn if a precompiled header is found in the search path but can't be used
    -Wlogical-op             # Warn about suspicious uses of logical operators in expressions
    -Wmissing-include-dirs   # Warn if a user-supplied include directory does not exist
    -Wpacked                 # Warn if a structure is given the packed attribute, but the packed attribute has no effect on the layout or size of the structure
    -Wredundant-decls        # Warn if anything is declared more than once in the same scope
    -Wswitch-default         # Warn whenever a switch statement does not have a default case
    -Wstrict-overflow=2      # Warn about cases where the compiler optimizes based on the assumption that signed overflow does not occur
    -Wshadow                 # Warn whenever a local variable or type declaration shadows another variable, parameter, type, or whenever a built-in function is shadowed
    -Wundef                  # Warn if an undefined identifier is evaluated in an '#if' directive
    -Wwrite-strings          # When compiling C, give string constants the type const char[length] so that copying the address of one into a non-const char * pointer produces a warning
    -Wno-packed              # Disable warn for a structure is given the packed attribute, but the packed attribute has no effect on the layout or size of the structure. The warning warns the code redundancy, disable it will have no harm.
)

add_compile_options(${COMPILE_OPTIONS_COMMON})

target_compile_options(app PRIVATE ${COMPILE_OPTIONS_COMMON})
