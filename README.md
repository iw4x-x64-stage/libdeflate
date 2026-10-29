# libdeflate - A C++ library

The `libdeflate` C++ library provides <SUMMARY-OF-FUNCTIONALITY>.


## Usage

To start using `libdeflate` in your project, add the following `depends`
value to your `manifest`, adjusting the version constraint as appropriate:

```
depends: libdeflate ^<VERSION>
```

Then import the library in your `buildfile`:

```
import libs = libdeflate%lib{<TARGET>}
```


## Importable targets

This package provides the following importable targets:

```
lib{<TARGET>}
```

<DESCRIPTION-OF-IMPORTABLE-TARGETS>


## Configuration variables

This package provides the following configuration variables:

```
[bool] config.libdeflate.<VARIABLE> ?= false
```

<DESCRIPTION-OF-CONFIG-VARIABLES>
