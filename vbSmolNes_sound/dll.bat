gcc -m32 -O3 -shared -o %1.dll %1.c %1.def -lwinmm -Wl,--enable-stdcall-fixup -static 
