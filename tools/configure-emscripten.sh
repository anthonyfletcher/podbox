# Sourced by tools/configure for --emscripten: simcc()'s setup for the
# simulator compiled to WebAssembly with Emscripten, for the Theme IDE.
# emcc, emar and emranlib must be on PATH (source emsdk_env.sh first).
#
# SDL2 is Emscripten's port, so there is no sdl-config to ask. A page has a
# single host thread, so the threads are fibers and need Asyncify; the
# asyncify stack holds a whole thread's calls while one sleeps.

emscripten_simcc () {
 app_type=$1
 winbuild=""
 macbuild=""
 arch=
 arch_version=
 ARG_ARM_THUMB=0

 CC=emcc
 CPP=cpp
 LD=emcc
 AR=emar
 AS=emcc
 RANLIB=emranlib
 OC=llvm-objcopy
 WINDRES=windres
 DLLTOOL=dlltool
 DLLWRAP=dllwrap

 GCCOPTS=`echo $CCOPTS | sed -e s/\ -ffreestanding// -e s/\ -nostdlib//`
 GCCOPTS="$GCCOPTS -fno-builtin -Wno-unused-result -sUSE_SDL=2"
 GCCOPTS="$GCCOPTS -I\$(SIMDIR)"
 GCCOPTIMIZE=''

 LDOPTS="$LDOPTS -Os -lm -sUSE_SDL=2 -sASYNCIFY -sASYNCIFY_STACK_SIZE=131072"
 LDOPTS="$LDOPTS -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=134217728"
 LDOPTS="$LDOPTS -sSTACK_SIZE=1048576 -sFORCE_FILESYSTEM"
 LDOPTS="$LDOPTS -sEXPORTED_RUNTIME_METHODS=FS,callMain,ccall -sINVOKE_RUN=0"
 # function names in the wasm, so a crash's stack says where it happened
 LDOPTS="$LDOPTS --profiling-funcs"

 output="rockboxui.js"
 SHARED_LDFLAGS="-sSIDE_MODULE"
 SHARED_CFLAGS="-fPIC"
 endian="little"
 thread_support="HAVE_EMSCRIPTEN_FIBER_THREADS"
 echo "Selected Emscripten fiber threads"
}
