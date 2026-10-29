./: {*/ -build/} doc{README.md NEWS} \
    legal{LICENSE.md LICENSE-EXCEPTION.md AUTHORS} manifest

# Don't install tests.
#
tests/: install = false
