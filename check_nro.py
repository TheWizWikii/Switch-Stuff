import struct, sys

path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

# NRO header: magic "NRO0" at offset 0x10, total file size (4 bytes) at offset 0x18
nro_magic = data[0x10:0x14]
if nro_magic != b"NRO0":
    print("No es un NRO valido (magic distinto):", nro_magic)
    sys.exit(1)

nro_size = struct.unpack_from("<I", data, 0x18)[0]
print(f"Tamano de NRO base (sin ASET): {nro_size} bytes")
print(f"Tamano total del archivo:      {len(data)} bytes")

aset_offset = nro_size
aset_magic = data[aset_offset:aset_offset+4]
print(f"Magic en offset ASET ({aset_offset}): {aset_magic}")

if aset_magic != b"ASET":
    print("NO se encontro seccion ASET -> el .nro no tiene icono/nacp/romfs empaquetados.")
    sys.exit(0)

version = struct.unpack_from("<I", data, aset_offset+4)[0]
icon_off, icon_size = struct.unpack_from("<QQ", data, aset_offset+8)
nacp_off, nacp_size = struct.unpack_from("<QQ", data, aset_offset+24)
romfs_off, romfs_size = struct.unpack_from("<QQ", data, aset_offset+40)

print(f"Icon:  offset={icon_off}  size={icon_size}")
print(f"NACP:  offset={nacp_off}  size={nacp_size}")
print(f"RomFS: offset={romfs_off}  size={romfs_size}")

if romfs_size == 0:
    print("\n>>> PROBLEMA CONFIRMADO: el romfs esta vacio o no se empaqueto (size=0).")
else:
    print(f"\n>>> RomFS SI esta empaquetado correctamente ({romfs_size} bytes).")
