# VicViewer Matchmaker

Small Node.js service that registers host sessions and resolves them for viewers.

## Scripts

- `npm install` – install dependencies
- `npm run build` – compile TypeScript to JavaScript
- `npm start` – start compiled server (listens on port 8787 by default)
- `npm run dev` – run development server with hot reload

## API

- `POST /register` – JSON body `{ code, address, port }`
- `GET /resolve?code=XXXX` – returns session info or 404
- `DELETE /register/:code` – remove session

WebSocket connections receive `registered`, `resolved`, `removed` events plus an initial snapshot.

## Despliegue en VPS (Ubuntu)

Script automatizado (ejecutar en el VPS dentro de la carpeta `matchmaker`):

```bash
chmod +x deploy/install_matchmaker_vps.sh
./deploy/install_matchmaker_vps.sh
```

Por defecto queda escuchando en `http://38.242.234.197:8787` y el cliente de la app de escritorio ahora usa esa URL como valor predefinido.

Variables de entorno ajustables editando el unit file systemd:

- `PORT` (default 8787)
- `SESSION_TTL_MS` (default 120000)
- `CLEANUP_INTERVAL_MS` (default 30000 via código si se exporta)
- `MATCHMAKER_SECRET` (opcional para restringir registros)

Para aplicar cambios:

```bash
sudo systemctl daemon-reload
sudo systemctl restart vicviewer-matchmaker.service
```
