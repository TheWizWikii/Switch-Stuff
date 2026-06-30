# switch-updater

Launcher homebrew simple para Nintendo Switch que descarga archivos (apps, ports, CFW...)
desde un JSON alojado en tu repo de GitHub.

## 1. Preparar tu repo de GitHub

1. Crea (o usa) un repositorio público en GitHub.
2. Sube un archivo `apps.json` (mira `apps.json.example` como plantilla) con esta forma:

```json
{
  "SECCION1": [
    { "name": "Nombre visible", "url": "https://...", "dest": "/ruta/en/la/sd/archivo.nro" }
  ],
  "SECCION2": [ ... ]
}
```

   - `name`: lo que se muestra en el menú.
   - `url`: link directo de descarga (recomendado: un "release asset" de GitHub, no el .zip del repo).
   - `dest`: ruta absoluta donde se guardará en la SD de la Switch (empieza con `/`).

3. Sube ese `apps.json` a `main` (o la rama que uses) y copia la URL "raw":
   `https://raw.githubusercontent.com/TU_USUARIO/TU_REPO/main/apps.json`

## 2. Configurar el proyecto

Edita `source/main.c` y cambia esta línea por tu URL real:

```c
#define JSON_URL "https://raw.githubusercontent.com/TU_USUARIO/TU_REPO/main/apps.json"
```

## 3. Añadir una fuente (.ttf)

Necesitas una fuente TrueType libre dentro de `romfs/fonts/font.ttf`. Por ejemplo, puedes
usar una fuente libre como Inter, Roboto, o la propia fuente de devkitPro de los ejemplos SDL2:

```bash
find /opt/devkitpro -iname "*.ttf"
```

Si encuentras una ahí, cópiala:
```bash
cp /opt/devkitpro/.../alguna.ttf romfs/fonts/font.ttf
```

Si no, descarga cualquier .ttf de una fuente libre (p. ej. Google Fonts) y colócala en
`romfs/fonts/font.ttf`.

## 4. Compilar

```bash
cd switch-updater
make
```

Si todo va bien, se genera `switch-updater.nro`.

## 5. Probar

- Copia `switch-updater.nro` a la carpeta `/switch/` de la tarjeta SD de tu Switch.
- Ábrelo desde el Homebrew Menu.
- Controles: D-Pad arriba/abajo para moverte por la sección actual, L/R (o D-Pad izq/der)
  para cambiar de sección, A para descargar el item seleccionado, B o + para salir.

También puedes probarlo sin consola física usando un emulador con soporte homebrew
(Ryujinx/yuzu) apuntando al .nro, aunque la red y el sistema de archivos a veces se
comportan distinto al hardware real — prueba en consola real antes de confiar del todo.

## Notas / mejoras futuras

- Ahora mismo `CURLOPT_SSL_VERIFYPEER` está desactivado (0L) porque validar certificados
  en Switch requiere un bundle de CAs (cacert.pem) cargado a mano. Es razonable para un
  proyecto personal, pero si quieres más seguridad, añade el cacert.pem a romfs y configura
  `CURLOPT_CAINFO`.
- El código actual no descomprime .zip (lo de `"extract": true` en el README inicial). Si
  quieres soporte de extracción automática (para CFW tipo Atmosphere), se puede añadir con
  `minizip` o `zziplib` (ya está en la lista de paquetes instalados) — dime si quieres que
  te añada esa parte.
- No hay manejo de "ya existe / sobrescribir" ni confirmación antes de descargar — para una
  v2 podría añadirse un diálogo de confirmación con A para no descargar sin querer.
