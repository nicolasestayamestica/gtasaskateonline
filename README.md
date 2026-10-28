# gtskate_net — fase 1 (espía)

Plugin `.asi` que NO cambia nada del juego: solo escribe `gtskate_net.log`
con lo que el mod de skate le pide a `skate_ffi.dll` (estado, pose de la tabla,
huesos). Con ese log confirmo los formatos antes de la fase 2 (enviar la tabla
por red).

## 1. Compilar con GitHub Actions
1. Crea un repositorio en github.com (puede ser privado).
2. Sube TODO el contenido de esta carpeta, incluida `.github/workflows/build.yml`
   (Add file > Upload files). Si GitHub no sube la carpeta `.github`, crea el
   archivo a mano: Add file > Create new file, nombre `.github/workflows/build.yml`,
   y pega el contenido.
3. Pestaña **Actions** > workflow **build** (se lanza solo al subir; si no,
   "Run workflow"). Tarda 1-3 minutos.
4. Entra a la ejecución terminada > **Artifacts** > descarga `gtskate_net`
   y saca `gtskate_net.asi` del zip.
   Si sale en rojo, copia el error del paso que falló y mándamelo.

## 2. Instalar y probar
1. Copia `gtskate_net.asi` a la carpeta del GTA, junto a `gtskate.asi`.
2. Abre el juego, conéctate al servidor y patina 2-3 minutos: suelo, salto,
   flips, grind, caídas, y camina un rato con la tabla en la mano.
3. Cierra el juego y manda `gtskate_net.log` (se crea junto al `.asi`).

Si el juego se cierra solo al arrancar: borra `gtskate_net.asi`, y mándame el
`gtskate_net.log` si se llegó a crear.
