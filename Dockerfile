FROM devkitpro/devkita64:latest

# Instalar todos los paquetes necesarios para compilar switch-updater.
# Esto se ejecuta UNA sola vez al construir la imagen, no en cada compilación.
RUN dkp-pacman --noconfirm -Sy \
    switch-curl \
    switch-mbedtls \
    switch-zlib \
    switch-sdl2 \
    switch-sdl2_image \
    switch-sdl2_ttf \
    switch-jansson \
    switch-bzip2 \
    switch-libpng \
    switch-freetype \
    switch-zziplib \
    && dkp-pacman -Scc --noconfirm
