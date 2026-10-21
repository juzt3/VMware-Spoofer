# LZMA SDK

Código vendorizado del **LZMA SDK 26.04** de 7-Zip (Igor Pavlov), dominio público.

- Origen: https://7-zip.org/sdk.html
- Archivo: `lzma2604.7z`, carpeta `C/`
- Licencia: public domain (ver cabeceras de cada archivo)

Archivos incluidos (los mínimos para `LzmaLib`):

```
7zTypes.h  7zWindows.h  Alloc.c/h  Compiler.h  CpuArch.c/h
LzFind.c/h  LzHash.h    LzmaDec.c/h  LzmaEnc.c/h  LzmaLib.c/h  Precomp.h
```

Se compila con `Z7_ST` / `_7ZIP_ST` para desactivar el match finder
multihilo (`LzFindMt`/`Threads` no se vendorizan). La compresión usa
`LzmaCompress(level=9, dict=16 MiB, lc=3, lp=0, pb=2, fb=64, threads=1)`,
que produce un stream LZMA1 "alone" con cabecera propia de 13 bytes.
