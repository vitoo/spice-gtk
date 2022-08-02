
build : 

    meson setup builddir
    cd builddir
    meson compile
    ./resize

