# VicViewer v17 - Auto-inicio con Email

## Nuevas Características

### 1. Auto-inicio desde nombre del ejecutable

El VicViewer ahora extrae automáticamente un **ClientID** del nombre del ejecutable:

- `VicViewer1234.exe` → ClientID: `1234`
- `VicViewer_ABC123.exe` → ClientID: `123` (solo dígitos finales)
- `VicViewer.exe` → Sin ClientID (comportamiento normal)

Cuando se detecta un ClientID, el programa **auto-inicia** la compartición de pantalla al arrancar.

### 2. Sistema de Email para Códigos de Acceso

Cuando un usuario con ClientID registrado inicia VicViewer:
1. Se genera el código de acceso automáticamente
2. El matchmaker busca el usuario en la base de datos
3. Si el usuario está **activo**, se envía un email con el código

### 3. Panel de Administración

Accede a `http://IP:8081/admin` para:
- Ver estadísticas de usuarios
- Crear/editar/eliminar usuarios
- Activar/desactivar usuarios

---

## Configuración del Servidor (Matchmaker)

### Requisitos
- Go 1.21+
- GCC (para SQLite con CGO)

### Compilar en Linux
```bash
cd /ruta/al/proyecto
CGO_ENABLED=1 go build -o cr-matchmaker ./cmd/cr-matchmaker
```

### Ejecutar
```bash
./cr-matchmaker -addr :8081
```

La base de datos SQLite se creará automáticamente en `matchmaker.db`.

### Configuración SMTP (en código)
```go
// pkg/matchmaker/email.go - DefaultEmailConfig()
Host:       "mail.sicaja.com"
Port:       465
Username:   "noresp@sicaja.com"
Password:   "Vescorp9454@1"
SenderName: "Vicviewer"
```

---

## Uso del Cliente VicViewer

### Modo Normal
Ejecutar `VicViewer.exe` - Funcionamiento estándar con botón "Compartir".

### Modo Auto-inicio
1. Renombrar el ejecutable agregando el ClientID:
   - `VicViewer1234.exe` (ClientID = 1234)
   
2. Al ejecutar, automáticamente:
   - Detecta ClientID `1234`
   - Inicia compartición de pantalla
   - Registra en matchmaker con clientId
   - Si hay usuario registrado, envía email

### Ejemplo de flujo
1. Administrador crea usuario en `/admin/users/new`:
   - ClientID: `1234`
   - Nombre: `Juan Pérez`
   - Email: `juan@empresa.com`
   
2. Distribuye `VicViewer1234.exe` al usuario Juan

3. Cuando Juan ejecuta el programa:
   - Se auto-inicia compartir pantalla
   - Juan recibe email con código de acceso
   - Soporte puede conectarse con ese código

---

## Panel de Administración

### Dashboard (`/admin`)
- Total de usuarios registrados
- Usuarios activos/deshabilitados
- Acciones rápidas

### Lista de Usuarios (`/admin/users`)
- Ver todos los usuarios
- Filtrar por estado
- Eliminar usuarios

### Crear Usuario (`/admin/users/new`)
- ClientID (número único, ej: 1234)
- Nombre completo
- Email
- Teléfono
- Fecha de activación
- Estado (activo/deshabilitado)

### Ver Usuario (`/admin/users/{id}`)
- Información completa
- Última conexión
- Nombre de ejecutable sugerido

---

## API REST

### Crear Sesión (con ClientID)
```bash
POST /v1/sessions
Content-Type: application/json

{
    "clientId": "1234"
}
```

Respuesta:
```json
{
    "code": "123456789",
    "expiresInMillis": 86400000,
    "emailSent": true
}
```

### Endpoints de Admin
- `GET /api/users` - Lista usuarios (JSON)
- `POST /api/users` - Crear usuario
- `PUT /api/users/{id}` - Actualizar usuario
- `DELETE /api/users/{id}` - Eliminar usuario

---

## Archivos del Paquete

```
VicViewer-v17-autostart/
├── VicViewer.exe         # Ejecutable principal
├── datachannel.dll       # WebRTC
├── jpeg62.dll            # JPEG encoding
├── libcrypto-3-x64.dll   # OpenSSL crypto
├── libssl-3-x64.dll      # OpenSSL SSL
└── libyuv.dll            # Color conversion
```

### Renombrar para auto-inicio:
```powershell
# Para cliente con ID 1234
Copy-Item VicViewer.exe -Destination VicViewer1234.exe
```

---

## Solución de Problemas

### Email no se envía
- Verificar credenciales SMTP en `email.go`
- Revisar logs del matchmaker
- Confirmar que el usuario está **activo** en la BD

### Auto-inicio no funciona
- Verificar que el nombre del exe termina en números
- Revisar logs del cliente (buscar "ClientID detectado")

### Panel admin no carga
- Verificar que los templates existen en `pkg/matchmaker/templates/`
- Recompilar matchmaker
