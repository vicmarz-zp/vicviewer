const express = require('express');
const Database = require('better-sqlite3');
const nodemailer = require('nodemailer');
const path = require('path');
const crypto = require('crypto');
const cookieParser = require('cookie-parser');
const multer = require('multer');
const fs = require('fs');

const app = express();
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(cookieParser());

// ============== CONFIGURACIÓN DE BANNERS ==============
const BANNERS_DIR = path.join(__dirname, 'banners');
if (!fs.existsSync(BANNERS_DIR)) {
    fs.mkdirSync(BANNERS_DIR, { recursive: true });
}

// Servir banners estáticamente
app.use('/banners', express.static(BANNERS_DIR));

// Configurar multer para subir banners
const bannerStorage = multer.diskStorage({
    destination: (req, file, cb) => cb(null, BANNERS_DIR),
    filename: (req, file, cb) => cb(null, 'temp_' + Date.now() + path.extname(file.originalname))
});
const uploadBanner = multer({
    storage: bannerStorage,
    limits: { fileSize: 2 * 1024 * 1024 }, // 2MB max
    fileFilter: (req, file, cb) => {
        const allowed = ['image/png', 'image/jpeg', 'image/jpg'];
        if (allowed.includes(file.mimetype)) {
            cb(null, true);
        } else {
            cb(new Error('Solo se permiten archivos PNG o JPEG'));
        }
    }
});

// ============== CONFIGURACIÓN ==============
const PORT = process.env.PORT || 8080;
const SESSION_TTL = 24 * 60 * 60 * 1000; // 24 horas
const ADMIN_SESSION_TTL = 8 * 60 * 60 * 1000; // 8 horas

// ============== BASE DE DATOS ==============
const db = new Database('matchmaker.db');
db.pragma('journal_mode = WAL');

db.exec(`
    -- Tabla de usuarios/clientes (empresas)
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        client_id TEXT UNIQUE NOT NULL,
        company_code TEXT UNIQUE,
        name TEXT NOT NULL,
        email TEXT UNIQUE NOT NULL,
        password_hash TEXT,
        phone TEXT DEFAULT '',
        company_name TEXT DEFAULT '',
        country TEXT DEFAULT '',
        state TEXT DEFAULT '',
        
        -- Permisos y configuración
        can_view_history INTEGER DEFAULT 0,
        can_view_devices INTEGER DEFAULT 0,
        has_subdomain INTEGER DEFAULT 0,
        
        -- Estado y fechas
        status TEXT DEFAULT 'pending',
        email_verified INTEGER DEFAULT 0,
        verification_token TEXT,
        reset_token TEXT,
        reset_token_expires TEXT,
        activation_date TEXT,
        last_connection TEXT,
        last_login TEXT,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT DEFAULT CURRENT_TIMESTAMP
    );
    CREATE INDEX IF NOT EXISTS idx_client_id ON users(client_id);
    CREATE INDEX IF NOT EXISTS idx_company_code ON users(company_code);
    CREATE INDEX IF NOT EXISTS idx_user_email ON users(email);
    
    -- Sesiones de usuarios (clientes)
    CREATE TABLE IF NOT EXISTS user_sessions (
        token TEXT PRIMARY KEY,
        user_id INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (user_id) REFERENCES users(id)
    );
    
    CREATE TABLE IF NOT EXISTS config (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL,
        updated_at TEXT DEFAULT CURRENT_TIMESTAMP
    );
    
    CREATE TABLE IF NOT EXISTS admin_sessions (
        token TEXT PRIMARY KEY,
        expires_at INTEGER NOT NULL,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP
    );
    
    CREATE TABLE IF NOT EXISTS connection_history (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        client_id TEXT NOT NULL,
        access_code TEXT NOT NULL,
        ip_address TEXT,
        user_agent TEXT,
        action TEXT DEFAULT 'connect',
        created_at TEXT DEFAULT CURRENT_TIMESTAMP
    );
    CREATE INDEX IF NOT EXISTS idx_history_client ON connection_history(client_id);
    CREATE INDEX IF NOT EXISTS idx_history_date ON connection_history(created_at);
    
    -- Tabla de dispositivos (equipos con servicio instalado)
    CREATE TABLE IF NOT EXISTS devices (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        client_id TEXT NOT NULL,
        device_code TEXT UNIQUE NOT NULL,
        device_name TEXT DEFAULT '',
        ip_address TEXT,
        is_online INTEGER DEFAULT 0,
        is_service INTEGER DEFAULT 0,
        last_seen TEXT,
        offer_sdp TEXT,
        ice_candidates TEXT,
        ice_servers TEXT,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (client_id) REFERENCES users(client_id)
    );
    CREATE INDEX IF NOT EXISTS idx_device_code ON devices(device_code);
    CREATE INDEX IF NOT EXISTS idx_device_client ON devices(client_id);
`);

// Migrar tabla users si faltan columnas
try {
    db.exec(`ALTER TABLE users ADD COLUMN company_code TEXT`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN password_hash TEXT`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN company_name TEXT DEFAULT ''`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN country TEXT DEFAULT ''`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN state TEXT DEFAULT ''`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN can_view_history INTEGER DEFAULT 0`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN can_view_devices INTEGER DEFAULT 0`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN has_subdomain INTEGER DEFAULT 0`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN email_verified INTEGER DEFAULT 0`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN verification_token TEXT`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN reset_token TEXT`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN reset_token_expires TEXT`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN last_login TEXT`);
} catch(e) {}
try {
    db.exec(`ALTER TABLE users ADD COLUMN custom_banner INTEGER DEFAULT 0`);
    console.log('[Migration] Columna custom_banner agregada');
} catch(e) { /* Ya existe */ }
try {
    db.exec(`CREATE TABLE IF NOT EXISTS user_sessions (
        token TEXT PRIMARY KEY,
        user_id INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP
    )`);
} catch(e) {}

// ============== CONFIGURACIÓN DE LOGS ==============
const LOG_LEVEL = process.env.LOG_LEVEL || 'info'; // 'debug', 'info', 'warn', 'error', 'none'
const LOG_LEVELS = { debug: 0, info: 1, warn: 2, error: 3, none: 99 };
const MAX_HISTORY_RECORDS = 100; // Máximo de registros de historial por usuario

function log(level, message) {
    if (LOG_LEVELS[level] >= LOG_LEVELS[LOG_LEVEL]) {
        const timestamp = new Date().toISOString().slice(11, 19);
        console.log(`[${timestamp}] ${message}`);
    }
}

// Limpiar historial: mantener solo los últimos MAX_HISTORY_RECORDS registros por usuario
function cleanupHistory() {
    const clients = db.prepare('SELECT DISTINCT client_id FROM connection_history').all();
    let totalDeleted = 0;
    for (const { client_id } of clients) {
        const result = db.prepare(`
            DELETE FROM connection_history 
            WHERE client_id = ? AND id NOT IN (
                SELECT id FROM connection_history 
                WHERE client_id = ? 
                ORDER BY created_at DESC 
                LIMIT ?
            )
        `).run(client_id, client_id, MAX_HISTORY_RECORDS);
        totalDeleted += result.changes;
    }
    if (totalDeleted > 0) {
        log('info', `🧹 ${totalDeleted} registros de historial limpiados (límite: ${MAX_HISTORY_RECORDS} por usuario)`);
    }
}

// Limpiar al iniciar
cleanupHistory();

// ============== FREE MODE CONFIGURATION ==============
const FREE_MODE_SESSION_DURATION = 90 * 1000; // 5 minutos
const FREE_MODE_COOLDOWN = 10 * 60 * 1000; // 10 minutos entre sesiones

// Migración para service_password
try {
    db.exec(`ALTER TABLE users ADD COLUMN service_password TEXT`);
    console.log('[Migration] Columna service_password agregada');
} catch(e) { /* Ya existe */ }

// Tabla para sesiones FREE mode
try {
    db.exec(`
        CREATE TABLE IF NOT EXISTS free_mode_sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            disk_serial TEXT NOT NULL,
            company_code TEXT,
            started_at TEXT NOT NULL,
            ended_at TEXT,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
    `);
    db.exec(`CREATE INDEX IF NOT EXISTS idx_fms_disk ON free_mode_sessions(disk_serial)`);
    db.exec(`CREATE INDEX IF NOT EXISTS idx_fms_ended ON free_mode_sessions(ended_at)`);
    console.log('[Migration] Tabla free_mode_sessions creada');
} catch(e) { /* Ya existe */ }

// Generar contraseña de servicio (5 caracteres alfanuméricos)
function generateServicePassword() {
    const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    let password = '';
    for (let i = 0; i < 5; i++) {
        password += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return password;
}

// Verificar estado FREE mode para un disk_serial
function checkFreeModeStatus(diskSerial, companyCode) {
    if (companyCode) {
        const user = db.prepare(`
            SELECT id, status, name, company_name 
            FROM users 
            WHERE company_code = ? AND status = 'active'
        `).get(companyCode);
        
        if (user) {
            return {
                allowed: true,
                isPaid: true,
                waitMinutes: 0,
                message: 'Cuenta activa',
                userName: user.name,
                companyName: user.company_name
            };
        }
    }
    
    const lastSession = db.prepare(`
        SELECT ended_at 
        FROM free_mode_sessions 
        WHERE disk_serial = ? AND ended_at IS NOT NULL
        ORDER BY ended_at DESC 
        LIMIT 1
    `).get(diskSerial);
    
    if (lastSession && lastSession.ended_at) {
        const endedAt = new Date(lastSession.ended_at).getTime();
        const now = Date.now();
        const elapsed = now - endedAt;
        
        if (elapsed < FREE_MODE_COOLDOWN) {
            const waitMs = FREE_MODE_COOLDOWN - elapsed;
            const waitMinutes = Math.ceil(waitMs / 60000);
            return {
                allowed: false,
                isPaid: false,
                waitMinutes: waitMinutes,
                message: `Debe esperar ${waitMinutes} minutos para iniciar otra sesión gratuita`
            };
        }
    }
    
    return {
        allowed: true,
        isPaid: false,
        waitMinutes: 0,
        message: 'Sesión gratuita disponible (máximo 5 minutos)'
    };
}

function startFreeSession(diskSerial, companyCode = null) {
    db.prepare(`
        INSERT INTO free_mode_sessions (disk_serial, company_code, started_at)
        VALUES (?, ?, datetime('now'))
    `).run(diskSerial, companyCode);
    log('info', `🆓 Sesión FREE iniciada: ${diskSerial.substring(0,8)}...`);
}

function endFreeSession(diskSerial) {
    db.prepare(`
        UPDATE free_mode_sessions 
        SET ended_at = datetime('now')
        WHERE disk_serial = ? AND ended_at IS NULL
    `).run(diskSerial);
    log('info', `🆓 Sesión FREE terminada: ${diskSerial.substring(0,8)}...`);
}


// Configuración inicial por defecto
function initConfig() {
    const defaults = {
        'admin_user': 'admin',
        'admin_pass_hash': crypto.createHash('sha256').update('admin123').digest('hex'),
        'smtp_host': 'mail.sicaja.com',
        'smtp_port': '465',
        'smtp_secure': 'true',
        'smtp_user': 'noresp@sicaja.com',
        'smtp_pass': 'Vescorp9454@1',
        'smtp_from_name': 'Vicviewer®',
        'smtp_from_email': 'noresp@sicaja.com'
    };
    
    for (const [key, value] of Object.entries(defaults)) {
        const exists = db.prepare('SELECT 1 FROM config WHERE key = ?').get(key);
        if (!exists) {
            db.prepare('INSERT INTO config (key, value) VALUES (?, ?)').run(key, value);
        }
    }
}
initConfig();

// Funciones de configuración
function getConfig(key) {
    const row = db.prepare('SELECT value FROM config WHERE key = ?').get(key);
    return row?.value;
}

function setConfig(key, value) {
    db.prepare('INSERT OR REPLACE INTO config (key, value, updated_at) VALUES (?, ?, ?)')
      .run(key, value, new Date().toISOString());
}

// ============== AUTENTICACIÓN ADMIN ==============
function generateToken() {
    return crypto.randomBytes(32).toString('hex');
}

function createAdminSession() {
    const token = generateToken();
    const expiresAt = Date.now() + ADMIN_SESSION_TTL;
    db.prepare('INSERT INTO admin_sessions (token, expires_at) VALUES (?, ?)').run(token, expiresAt);
    // Limpiar sesiones expiradas
    db.prepare('DELETE FROM admin_sessions WHERE expires_at < ?').run(Date.now());
    return token;
}

function validateAdminSession(token) {
    if (!token) return false;
    const session = db.prepare('SELECT * FROM admin_sessions WHERE token = ? AND expires_at > ?').get(token, Date.now());
    return !!session;
}

function authMiddleware(req, res, next) {
    const token = req.cookies?.admin_token;
    if (!validateAdminSession(token)) {
        return res.redirect('/admin/login');
    }
    next();
}

// ============== AUTENTICACIÓN USUARIOS (CLIENTES) ==============
const USER_SESSION_TTL = 24 * 60 * 60 * 1000; // 24 horas

function createUserSession(userId) {
    const token = generateToken();
    const expiresAt = Date.now() + USER_SESSION_TTL;
    db.prepare('INSERT INTO user_sessions (token, user_id, expires_at) VALUES (?, ?, ?)').run(token, userId, expiresAt);
    db.prepare('DELETE FROM user_sessions WHERE expires_at < ?').run(Date.now());
    return token;
}

function validateUserSession(token) {
    if (!token) return null;
    const session = db.prepare(`
        SELECT us.*, u.* FROM user_sessions us 
        JOIN users u ON us.user_id = u.id 
        WHERE us.token = ? AND us.expires_at > ?
    `).get(token, Date.now());
    return session;
}

function userAuthMiddleware(req, res, next) {
    const token = req.cookies?.user_token;
    const user = validateUserSession(token);
    if (!user) {
        return res.redirect('/login');
    }
    req.user = user;
    next();
}

// Generar código de empresa (4 caracteres alfanuméricos)
function generateCompanyCode() {
    const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    let code;
    let attempts = 0;
    do {
        code = Array.from({length: 4}, () => chars[Math.floor(Math.random() * chars.length)]).join('');
        attempts++;
    } while (db.prepare('SELECT 1 FROM users WHERE company_code = ?').get(code) && attempts < 100);
    return code;
}

// Generar token de verificación
function generateVerificationToken() {
    return crypto.randomBytes(20).toString('hex');
}

// ============== EMAIL ==============
function createTransporter() {
    return nodemailer.createTransport({
        host: getConfig('smtp_host'),
        port: parseInt(getConfig('smtp_port')),
        secure: getConfig('smtp_secure') === 'true',
        auth: {
            user: getConfig('smtp_user'),
            pass: getConfig('smtp_pass')
        }
    });
}

async function sendAccessCode(email, name, code) {
    const fromName = getConfig('smtp_from_name');
    const fromEmail = getConfig('smtp_from_email');
    
    const html = `
    <div style="font-family: 'Segoe UI', Arial; background: #1a1a2e; color: #eee; padding: 30px; max-width: 500px; margin: auto; border-radius: 12px;">
        <div style="text-align: center; margin-bottom: 25px;">
            <div style="font-size: 28px; font-weight: bold; color: #00d4ff;">🖥️ Vicviewer®</div>
            <p style="color: #888;">Control Remoto Seguro</p>
        </div>
        <p>Hola <strong>${name}</strong>,</p>
        <p>Se ha iniciado una sesión de acceso remoto:</p>
        <div style="background: #0f3460; border: 2px solid #00d4ff; border-radius: 10px; padding: 20px; text-align: center; margin: 20px 0;">
            <div style="color: #888; font-size: 12px; text-transform: uppercase;">Código de Acceso</div>
            <div style="font-size: 36px; font-weight: bold; color: #00d4ff; letter-spacing: 8px; font-family: Consolas;">${code}</div>
        </div>
        <div style="background: #2d1f1f; border-left: 4px solid #ff6b6b; padding: 12px; color: #ffaaaa; font-size: 13px;">
            ⚠️ Válido por 24 horas. No compartir.
        </div>
        <p style="color: #aaa; font-size: 14px; margin-top: 20px;">
            <strong>¿Cómo conectarse?</strong><br>
            1. Abre Vicviewer® → 2. Pestaña "Viewer" → 3. Ingresa el código
        </p>
    </div>`;

    const transporter = createTransporter();
    await transporter.sendMail({
        from: `"${fromName}" <${fromEmail}>`,
        to: email,
        subject: '🔐 Código de Acceso Remoto - Vicviewer®',
        html
    });
}

async function sendVerificationEmail(email, name, token) {
    const fromName = getConfig('smtp_from_name');
    const fromEmail = getConfig('smtp_from_email');
    const baseUrl = getConfig('base_url') || 'http://38.242.234.197:8080';
    const verifyUrl = `${baseUrl}/verify-email?token=${token}`;
    
    const html = `
    <div style="font-family: 'Segoe UI', Arial; background: #1a1a2e; color: #eee; padding: 30px; max-width: 500px; margin: auto; border-radius: 12px;">
        <div style="text-align: center; margin-bottom: 25px;">
            <div style="font-size: 28px; font-weight: bold; color: #00d4ff;">🖥️ Vicviewer®</div>
            <p style="color: #888;">Plataforma de Control Remoto</p>
        </div>
        <p>Hola <strong>${name}</strong>,</p>
        <p>Gracias por registrarte en Vicviewer®. Por favor confirma tu correo electrónico:</p>
        <div style="text-align: center; margin: 30px 0;">
            <a href="${verifyUrl}" style="background: #00d4ff; color: #000; padding: 15px 30px; text-decoration: none; border-radius: 8px; font-weight: bold; display: inline-block;">
                ✅ Verificar mi correo
            </a>
        </div>
        <p style="color: #888; font-size: 13px;">O copia este enlace: <br><span style="color:#00d4ff">${verifyUrl}</span></p>
        <p style="color: #666; font-size: 12px; margin-top: 30px;">Si no creaste esta cuenta, ignora este mensaje.</p>
    </div>`;

    const transporter = createTransporter();
    await transporter.sendMail({
        from: `"${fromName}" <${fromEmail}>`,
        to: email,
        subject: '✅ Confirma tu correo - Vicviewer®',
        html
    });
}

async function sendPasswordResetEmail(email, name, token) {
    const fromName = getConfig('smtp_from_name');
    const fromEmail = getConfig('smtp_from_email');
    const baseUrl = getConfig('base_url') || 'http://38.242.234.197:8080';
    const resetUrl = `${baseUrl}/reset-password?token=${token}`;
    
    const html = `
    <div style="font-family: 'Segoe UI', Arial; background: #1a1a2e; color: #eee; padding: 30px; max-width: 500px; margin: auto; border-radius: 12px;">
        <div style="text-align: center; margin-bottom: 25px;">
            <div style="font-size: 28px; font-weight: bold; color: #00d4ff;">🖥️ Vicviewer®</div>
        </div>
        <p>Hola <strong>${name}</strong>,</p>
        <p>Recibimos una solicitud para restablecer tu contraseña:</p>
        <div style="text-align: center; margin: 30px 0;">
            <a href="${resetUrl}" style="background: #ff6b6b; color: #fff; padding: 15px 30px; text-decoration: none; border-radius: 8px; font-weight: bold; display: inline-block;">
                🔑 Restablecer contraseña
            </a>
        </div>
        <p style="color: #888; font-size: 13px;">Este enlace expira en 1 hora.</p>
        <p style="color: #666; font-size: 12px; margin-top: 30px;">Si no solicitaste esto, ignora este mensaje.</p>
    </div>`;

    const transporter = createTransporter();
    await transporter.sendMail({
        from: `"${fromName}" <${fromEmail}>`,
        to: email,
        subject: '🔑 Restablecer contraseña - Vicviewer®',
        html
    });
}

// Sesiones en memoria
const sessions = new Map();

// ============== API MATCHMAKER ==============

// ============== MIDDLEWARE DE SUBDOMINIOS ==============
// Detecta subdominios como abc1.vicviewer.com y muestra landing personalizada
app.use((req, res, next) => {
    const host = req.hostname || req.headers.host?.split(':')[0] || '';
    const parts = host.split('.');
    
    // Si es un subdominio (ej: abc1.vicviewer.com tiene 3 partes)
    if (parts.length >= 3 && parts[1] === 'vicviewer' && parts[2] === 'com') {
        const companyCode = parts[0].toUpperCase();
        
        // Buscar usuario con ese código de empresa
        const user = db.prepare('SELECT * FROM users WHERE company_code = ? AND status = ? AND has_subdomain = 1')
            .get(companyCode, 'active');
        
        if (user) {
            req.subdomainUser = user;
            req.companyCode = companyCode;
        }
    }
    next();
});

// Landing page para subdominios
const subdomainLandingCSS = `
<style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: 'Segoe UI', sans-serif; background: linear-gradient(135deg, #0f0f1a 0%, #1a1a2e 50%, #0f3460 100%); color: #eee; min-height: 100vh; display: flex; flex-direction: column; }
    .hero { flex: 1; display: flex; flex-direction: column; justify-content: center; align-items: center; text-align: center; padding: 40px 20px; }
    .logo { font-size: 48px; margin-bottom: 10px; }
    .company-name { font-size: 36px; font-weight: bold; color: #00d4ff; margin-bottom: 10px; }
    .tagline { font-size: 18px; color: #888; margin-bottom: 40px; }
    .code-display { background: linear-gradient(135deg, #16213e 0%, #0f3460 100%); border: 2px solid #00d4ff; border-radius: 16px; padding: 30px 50px; margin-bottom: 40px; }
    .code-label { color: #888; font-size: 14px; margin-bottom: 10px; }
    .code { font-size: 64px; font-weight: bold; color: #fff; letter-spacing: 12px; font-family: 'Consolas', monospace; text-shadow: 0 0 20px rgba(0,212,255,0.5); }
    .buttons { display: flex; gap: 20px; flex-wrap: wrap; justify-content: center; }
    .btn { padding: 18px 40px; border: none; border-radius: 12px; font-size: 18px; font-weight: 600; cursor: pointer; text-decoration: none; display: inline-flex; align-items: center; gap: 10px; transition: all 0.3s; }
    .btn-primary { background: linear-gradient(135deg, #00d4ff 0%, #0099cc 100%); color: #000; }
    .btn-primary:hover { transform: translateY(-3px); box-shadow: 0 10px 30px rgba(0,212,255,0.4); }
    .btn-secondary { background: transparent; border: 2px solid #00d4ff; color: #00d4ff; }
    .btn-secondary:hover { background: #00d4ff22; }
    .features { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 30px; max-width: 900px; margin-top: 60px; }
    .feature { background: #1a1a2e88; border-radius: 12px; padding: 25px; text-align: center; }
    .feature-icon { font-size: 36px; margin-bottom: 15px; }
    .feature h3 { color: #00d4ff; margin-bottom: 10px; }
    .feature p { color: #888; font-size: 14px; }
    .footer { text-align: center; padding: 20px; color: #666; font-size: 12px; border-top: 1px solid #333; }
</style>`;

// ============== LANDING PAGE PRINCIPAL ==============
const mainLandingHTML = `<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Vicviewer® - Software de Escritorio Remoto Profesional</title>
    <meta name="description" content="Vicviewer® es la solución profesional de escritorio remoto con conexión instantánea, máxima seguridad y la menor latencia del mercado. Ideal para soporte técnico y trabajo remoto.">
    <link rel="icon" type="image/svg+xml" href="/favicon.svg">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        html { scroll-behavior: smooth; }
        body { font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, sans-serif; background: #0a0a12; color: #e8e8e8; line-height: 1.6; }
        
        /* Header */
        .header { position: fixed; top: 0; left: 0; right: 0; z-index: 1000; background: rgba(10,10,18,0.95); backdrop-filter: blur(20px); border-bottom: 1px solid #1a1a2e; }
        .nav { max-width: 1200px; margin: 0 auto; padding: 15px 30px; display: flex; justify-content: space-between; align-items: center; }
        .nav-logo { font-size: 24px; font-weight: 700; color: #00d4ff; text-decoration: none; display: flex; align-items: center; gap: 10px; }
        .nav-logo svg { width: 32px; height: 32px; }
        .nav-links { display: flex; gap: 30px; align-items: center; }
        .nav-links a { color: #888; text-decoration: none; font-size: 14px; transition: color 0.3s; }
        .nav-links a:hover { color: #00d4ff; }
        .nav-btn { background: linear-gradient(135deg, #00d4ff 0%, #0099cc 100%); color: #000; padding: 10px 24px; border-radius: 8px; font-weight: 600; text-decoration: none; transition: all 0.3s; }
        .nav-btn:hover { transform: translateY(-2px); box-shadow: 0 8px 25px rgba(0,212,255,0.3); color: #000; }
        
        /* Hero Section */
        .hero { min-height: 100vh; display: flex; align-items: center; justify-content: center; text-align: center; padding: 120px 30px 80px; background: radial-gradient(ellipse at top, #0f3460 0%, #0a0a12 60%); position: relative; overflow: hidden; }
        .hero::before { content: ''; position: absolute; top: 0; left: 0; right: 0; bottom: 0; background: url("data:image/svg+xml,%3Csvg width='60' height='60' viewBox='0 0 60 60' xmlns='http://www.w3.org/2000/svg'%3E%3Cg fill='none' fill-rule='evenodd'%3E%3Cg fill='%2300d4ff' fill-opacity='0.03'%3E%3Cpath d='M36 34v-4h-2v4h-4v2h4v4h2v-4h4v-2h-4zm0-30V0h-2v4h-4v2h4v4h2V6h4V4h-4zM6 34v-4H4v4H0v2h4v4h2v-4h4v-2H6zM6 4V0H4v4H0v2h4v4h2V6h4V4H6z'/%3E%3C/g%3E%3C/g%3E%3C/svg%3E"); opacity: 0.5; }
        .hero-content { position: relative; z-index: 1; max-width: 900px; }
        .hero-badge { display: inline-block; background: linear-gradient(135deg, #00d4ff22, #0099cc22); border: 1px solid #00d4ff44; color: #00d4ff; padding: 8px 20px; border-radius: 50px; font-size: 13px; font-weight: 500; margin-bottom: 30px; }
        .hero h1 { font-size: clamp(36px, 6vw, 64px); font-weight: 800; line-height: 1.1; margin-bottom: 25px; background: linear-gradient(135deg, #fff 0%, #00d4ff 100%); -webkit-background-clip: text; -webkit-text-fill-color: transparent; background-clip: text; }
        .hero-subtitle { font-size: clamp(16px, 2.5vw, 22px); color: #888; max-width: 650px; margin: 0 auto 40px; }
        .hero-buttons { display: flex; gap: 20px; justify-content: center; flex-wrap: wrap; }
        .btn { padding: 16px 36px; border-radius: 12px; font-size: 16px; font-weight: 600; text-decoration: none; display: inline-flex; align-items: center; gap: 10px; transition: all 0.3s; border: none; cursor: pointer; }
        .btn-primary { background: linear-gradient(135deg, #00d4ff 0%, #0099cc 100%); color: #000; }
        .btn-primary:hover { transform: translateY(-3px); box-shadow: 0 15px 40px rgba(0,212,255,0.4); }
        .btn-outline { background: transparent; border: 2px solid #333; color: #fff; }
        .btn-outline:hover { border-color: #00d4ff; color: #00d4ff; }
        .hero-stats { display: flex; gap: 50px; justify-content: center; margin-top: 60px; flex-wrap: wrap; }
        .stat { text-align: center; }
        .stat-value { font-size: 36px; font-weight: 700; color: #00d4ff; }
        .stat-label { font-size: 13px; color: #666; margin-top: 5px; }
        
        /* Features Section */
        .features { padding: 100px 30px; background: #0a0a12; }
        .container { max-width: 1200px; margin: 0 auto; }
        .section-header { text-align: center; margin-bottom: 60px; }
        .section-header h2 { font-size: 36px; font-weight: 700; margin-bottom: 15px; }
        .section-header p { color: #666; font-size: 18px; max-width: 600px; margin: 0 auto; }
        .features-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 30px; }
        .feature-card { background: linear-gradient(135deg, #12121f 0%, #1a1a2e 100%); border: 1px solid #1f1f3a; border-radius: 16px; padding: 35px; transition: all 0.4s; }
        .feature-card:hover { transform: translateY(-8px); border-color: #00d4ff33; box-shadow: 0 20px 50px rgba(0,0,0,0.3); }
        .feature-icon { width: 60px; height: 60px; background: linear-gradient(135deg, #00d4ff22, #0099cc11); border-radius: 14px; display: flex; align-items: center; justify-content: center; font-size: 28px; margin-bottom: 20px; }
        .feature-card h3 { font-size: 20px; font-weight: 600; margin-bottom: 12px; color: #fff; }
        .feature-card p { color: #777; font-size: 15px; line-height: 1.7; }
        
        /* How it Works */
        .how-it-works { padding: 100px 30px; background: linear-gradient(180deg, #0a0a12 0%, #0f1525 100%); }
        .steps { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 40px; margin-top: 50px; }
        .step { text-align: center; position: relative; }
        .step-number { width: 70px; height: 70px; background: linear-gradient(135deg, #00d4ff 0%, #0099cc 100%); border-radius: 50%; display: flex; align-items: center; justify-content: center; font-size: 28px; font-weight: 700; color: #000; margin: 0 auto 25px; }
        .step h3 { font-size: 20px; margin-bottom: 12px; }
        .step p { color: #666; font-size: 15px; }
        
        /* Use Cases */
        .use-cases { padding: 100px 30px; background: #0a0a12; }
        .cases-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 30px; }
        .case-card { background: linear-gradient(135deg, #16213e 0%, #1a1a2e 100%); border-radius: 16px; padding: 40px; border: 1px solid #1f2f4f; }
        .case-card h3 { font-size: 22px; margin-bottom: 15px; color: #00d4ff; }
        .case-card p { color: #888; margin-bottom: 20px; }
        .case-card ul { list-style: none; }
        .case-card li { padding: 8px 0; color: #aaa; font-size: 14px; display: flex; align-items: center; gap: 10px; }
        .case-card li::before { content: '✓'; color: #10b981; font-weight: bold; }
        
        /* Pricing */
        .pricing { padding: 100px 30px; background: linear-gradient(180deg, #0f1525 0%, #0a0a12 100%); }
        .pricing-cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 30px; max-width: 900px; margin: 0 auto; }
        .pricing-card { background: #12121f; border: 1px solid #1f1f3a; border-radius: 20px; padding: 40px; text-align: center; position: relative; }
        .pricing-card.featured { border-color: #00d4ff; transform: scale(1.05); }
        .pricing-card.featured::before { content: 'MÁS POPULAR'; position: absolute; top: -12px; left: 50%; transform: translateX(-50%); background: linear-gradient(135deg, #00d4ff, #0099cc); color: #000; padding: 6px 20px; border-radius: 20px; font-size: 11px; font-weight: 700; }
        .pricing-card h3 { font-size: 24px; margin-bottom: 10px; }
        .pricing-card .price { font-size: 48px; font-weight: 700; color: #00d4ff; margin: 20px 0; }
        .pricing-card .price span { font-size: 16px; color: #666; }
        .pricing-card ul { list-style: none; text-align: left; margin: 30px 0; }
        .pricing-card li { padding: 12px 0; color: #888; border-bottom: 1px solid #1f1f3a; display: flex; align-items: center; gap: 10px; }
        .pricing-card li::before { content: '✓'; color: #00d4ff; }
        
        /* CTA Section */
        .cta { padding: 100px 30px; background: linear-gradient(135deg, #0f3460 0%, #16213e 100%); text-align: center; }
        .cta h2 { font-size: 40px; font-weight: 700; margin-bottom: 20px; }
        .cta p { color: #888; font-size: 18px; margin-bottom: 40px; max-width: 600px; margin-left: auto; margin-right: auto; }
        
        /* Footer */
        .footer { background: #050508; padding: 60px 30px 30px; }
        .footer-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 40px; max-width: 1200px; margin: 0 auto 40px; }
        .footer-col h4 { color: #fff; font-size: 16px; margin-bottom: 20px; }
        .footer-col a { display: block; color: #666; text-decoration: none; padding: 8px 0; font-size: 14px; transition: color 0.3s; }
        .footer-col a:hover { color: #00d4ff; }
        .footer-bottom { border-top: 1px solid #1a1a2e; padding-top: 30px; text-align: center; max-width: 1200px; margin: 0 auto; }
        .footer-bottom p { color: #444; font-size: 13px; }
        .footer-bottom a { color: #555; text-decoration: none; margin: 0 15px; }
        .footer-bottom a:hover { color: #00d4ff; }
        
        /* Responsive */
        @media (max-width: 768px) {
            .nav-links { display: none; }
            .hero-stats { gap: 30px; }
            .stat-value { font-size: 28px; }
        }
    </style>
</head>
<body>
    <!-- Header -->
    <header class="header">
        <nav class="nav">
            <a href="/" class="nav-logo">
                <svg viewBox="0 0 100 100" fill="none" xmlns="http://www.w3.org/2000/svg">
                    <rect width="100" height="100" rx="20" fill="#00d4ff"/>
                    <rect x="15" y="15" width="70" height="50" rx="8" fill="white"/>
                    <rect x="30" y="72" width="40" height="8" rx="4" fill="white"/>
                </svg>
                Vicviewer®
            </a>
            <div class="nav-links">
                <a href="#features">Características</a>
                <a href="#how-it-works">Cómo Funciona</a>
                <a href="#use-cases">Casos de Uso</a>
                <a href="#pricing">Precios</a>
                <a href="#contact">Contacto</a>
                <a href="/login" class="nav-btn">Iniciar Sesión</a>
            </div>
        </nav>
    </header>

    <!-- Hero Section -->
    <section class="hero">
        <div class="hero-content">
            <span class="hero-badge">🚀 Tecnología WebRTC de Nueva Generación</span>
            <h1>Control Remoto Instantáneo, Seguro y Sin Límites</h1>
            <p class="hero-subtitle">La solución profesional de escritorio remoto que tu empresa necesita. Conexión en segundos, cifrado de extremo a extremo y la menor latencia del mercado.</p>
            <div class="hero-buttons">
                <a href="/login" class="btn btn-primary">
                    👤 Acceder al Panel
                </a>
                <a href="#how-it-works" class="btn btn-outline">
                    Ver Cómo Funciona
                </a>
            </div>
            <div class="hero-stats">
                <div class="stat">
                    <div class="stat-value">&lt;50ms</div>
                    <div class="stat-label">Latencia Promedio</div>
                </div>
                <div class="stat">
                    <div class="stat-value">256-bit</div>
                    <div class="stat-label">Cifrado AES</div>
                </div>
                <div class="stat">
                    <div class="stat-value">99.9%</div>
                    <div class="stat-label">Uptime Garantizado</div>
                </div>
            </div>
        </div>
    </section>

    <!-- Features Section -->
    <section class="features" id="features">
        <div class="container">
            <div class="section-header">
                <h2>¿Por qué elegir Vicviewer®?</h2>
                <p>Diseñado para profesionales que exigen rendimiento, seguridad y simplicidad</p>
            </div>
            <div class="features-grid">
                <div class="feature-card">
                    <div class="feature-icon">⚡</div>
                    <h3>Conexión Instantánea</h3>
                    <p>Tecnología WebRTC peer-to-peer que establece conexiones directas en menos de 3 segundos. Sin esperas, sin configuraciones complicadas.</p>
                </div>
                <div class="feature-card">
                    <div class="feature-icon">🔒</div>
                    <h3>Seguridad Empresarial</h3>
                    <p>Cifrado AES-256 de extremo a extremo. Tus sesiones son privadas y seguras. Cumple con estándares SOC 2 y GDPR.</p>
                </div>
                <div class="feature-card">
                    <div class="feature-icon">🌐</div>
                    <h3>Funciona en Cualquier Red</h3>
                    <p>Nuestra tecnología TURN/STUN atraviesa firewalls y NAT automáticamente. Funciona en redes corporativas restrictivas.</p>
                </div>
                <div class="feature-card">
                    <div class="feature-icon">📺</div>
                    <h3>Calidad Ultra HD</h3>
                    <p>Streaming adaptativo que ajusta la calidad según tu conexión. Soporte para múltiples monitores y resoluciones 4K.</p>
                </div>
                <div class="feature-card">
                    <div class="feature-icon">🖱️</div>
                    <h3>Control Total</h3>
                    <p>Mouse, teclado, transferencia de archivos y portapapeles compartido. Todo lo que necesitas para trabajar eficientemente.</p>
                </div>
                <div class="feature-card">
                    <div class="feature-icon">🏢</div>
                    <h3>Multi-Empresa</h3>
                    <p>Cada cliente obtiene su propio código de empresa y subdominio personalizado. Perfecto para MSPs y empresas de soporte.</p>
                </div>
            </div>
        </div>
    </section>

    <!-- How it Works -->
    <section class="how-it-works" id="how-it-works">
        <div class="container">
            <div class="section-header">
                <h2>Tan Simple Como 1-2-3</h2>
                <p>Comienza a dar soporte remoto en menos de un minuto</p>
            </div>
            <div class="steps">
                <div class="step">
                    <div class="step-number">1</div>
                    <h3>Descarga el Software</h3>
                    <p>Tu cliente descarga Vicviewer® desde tu enlace personalizado. No requiere instalación ni permisos de administrador.</p>
                </div>
                <div class="step">
                    <div class="step-number">2</div>
                    <h3>Comparte el Código</h3>
                    <p>Al ejecutar, se genera un código único de 6 dígitos. Tu cliente te lo comparte por teléfono, chat o email.</p>
                </div>
                <div class="step">
                    <div class="step-number">3</div>
                    <h3>Conéctate al Instante</h3>
                    <p>Ingresa el código en tu visor y en segundos estarás viendo y controlando el equipo remoto. Así de fácil.</p>
                </div>
            </div>
        </div>
    </section>

    <!-- Use Cases -->
    <section class="use-cases" id="use-cases">
        <div class="container">
            <div class="section-header">
                <h2>Soluciones para Cada Necesidad</h2>
                <p>Vicviewer® se adapta a tu modelo de negocio</p>
            </div>
            <div class="cases-grid">
                <div class="case-card">
                    <h3>🛠️ Soporte Técnico</h3>
                    <p>Resuelve problemas técnicos sin desplazamientos. Ideal para help desks y equipos de TI.</p>
                    <ul>
                        <li>Diagnóstico remoto en tiempo real</li>
                        <li>Instalación y configuración de software</li>
                        <li>Resolución de incidencias al instante</li>
                        <li>Historial completo de sesiones</li>
                    </ul>
                </div>
                <div class="case-card">
                    <h3>💼 Trabajo Remoto</h3>
                    <p>Accede a tu computadora de oficina desde cualquier lugar del mundo.</p>
                    <ul>
                        <li>Acceso a archivos y aplicaciones</li>
                        <li>Conexión segura desde casa</li>
                        <li>Sin VPN complicadas</li>
                        <li>Productividad sin interrupciones</li>
                    </ul>
                </div>
                <div class="case-card">
                    <h3>🏫 Capacitación</h3>
                    <p>Enseña y entrena de forma interactiva tomando control de la pantalla del alumno.</p>
                    <ul>
                        <li>Demostraciones en vivo</li>
                        <li>Corrección de ejercicios en tiempo real</li>
                        <li>Clases particulares efectivas</li>
                        <li>Onboarding de empleados</li>
                    </ul>
                </div>
            </div>
        </div>
    </section>

    <!-- Pricing -->
    <section class="pricing" id="pricing">
        <div class="container">
            <div class="section-header">
                <h2>Planes Simples y Transparentes</h2>
                <p>Sin costos ocultos. Cancela cuando quieras.</p>
            </div>
            <div class="pricing-cards">
                <div class="pricing-card">
                    <h3>Básico</h3>
                    <div class="price">Gratis</div>
                    <p style="color:#666">Para uso personal</p>
                    <ul>
                        <li>Conexiones ilimitadas</li>
                        <li>1 dispositivo registrado</li>
                        <li>Soporte por email</li>
                        <li>Cifrado de extremo a extremo</li>
                    </ul>
                    <a href="/register" class="btn btn-outline" style="width:100%;justify-content:center;margin-top:20px">Comenzar Gratis</a>
                </div>
                <div class="pricing-card featured">
                    <h3>Profesional</h3>
                    <div class="price">$29<span>/mes</span></div>
                    <p style="color:#666">Para equipos de soporte</p>
                    <ul>
                        <li>Todo lo del plan Básico</li>
                        <li>10 dispositivos registrados</li>
                        <li>Subdominio personalizado</li>
                        <li>Panel de administración</li>
                        <li>Historial de conexiones</li>
                        <li>Soporte prioritario</li>
                    </ul>
                    <a href="/register" class="btn btn-primary" style="width:100%;justify-content:center;margin-top:20px">Comenzar Prueba</a>
                </div>
            </div>
        </div>
    </section>

    <!-- CTA -->
    <section class="cta">
        <h2>¿Listo para Revolucionar tu Soporte Remoto?</h2>
        <p>Únete a miles de profesionales que ya confían en Vicviewer® para conectarse con sus clientes.</p>
        <a href="/register" class="btn btn-primary" style="font-size:18px;padding:18px 50px">
            🚀 Crear Cuenta Gratis
        </a>
    </section>

    <!-- Contact Section -->
    <section class="contact" id="contact" style="padding:80px 30px;background:#0f0f1a">
        <div class="container" style="max-width:600px;margin:0 auto">
            <div class="section-header" style="text-align:center;margin-bottom:40px">
                <h2 style="font-size:36px;color:#fff;margin-bottom:15px">📬 Contáctanos</h2>
                <p style="color:#888">¿Tienes preguntas? Escríbenos y te responderemos lo antes posible.</p>
            </div>
            <form id="contactForm" style="background:#1a1a2e;padding:30px;border-radius:12px;border:1px solid #2a2a4a">
                <div style="margin-bottom:20px">
                    <label style="display:block;color:#888;margin-bottom:8px;font-size:14px">Nombre *</label>
                    <input type="text" name="name" required placeholder="Tu nombre" style="width:100%;padding:12px;background:#0f0f1a;border:1px solid #333;border-radius:8px;color:#fff;font-size:14px">
                </div>
                <div style="margin-bottom:20px">
                    <label style="display:block;color:#888;margin-bottom:8px;font-size:14px">Email *</label>
                    <input type="email" name="email" required placeholder="tu@email.com" style="width:100%;padding:12px;background:#0f0f1a;border:1px solid #333;border-radius:8px;color:#fff;font-size:14px">
                </div>
                <div style="margin-bottom:20px">
                    <label style="display:block;color:#888;margin-bottom:8px;font-size:14px">Asunto *</label>
                    <input type="text" name="subject" required placeholder="Asunto del mensaje" style="width:100%;padding:12px;background:#0f0f1a;border:1px solid #333;border-radius:8px;color:#fff;font-size:14px">
                </div>
                <div style="margin-bottom:20px">
                    <label style="display:block;color:#888;margin-bottom:8px;font-size:14px">Mensaje *</label>
                    <textarea name="message" required rows="5" placeholder="Escribe tu mensaje aquí..." style="width:100%;padding:12px;background:#0f0f1a;border:1px solid #333;border-radius:8px;color:#fff;font-size:14px;resize:vertical"></textarea>
                </div>
                <button type="submit" style="width:100%;padding:15px;background:linear-gradient(135deg,#00d4ff 0%,#0099cc 100%);color:#000;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;transition:all 0.3s">
                    ✉️ Enviar Mensaje
                </button>
                <div id="contactResult" style="margin-top:15px;text-align:center;display:none"></div>
            </form>
        </div>
    </section>
    <script>
        document.getElementById('contactForm').addEventListener('submit', async function(e) {
            e.preventDefault();
            const btn = this.querySelector('button');
            const result = document.getElementById('contactResult');
            btn.disabled = true;
            btn.textContent = '⏳ Enviando...';
            result.style.display = 'none';
            
            try {
                const formData = new FormData(this);
                const data = Object.fromEntries(formData);
                const res = await fetch('/api/contact', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(data)
                });
                const json = await res.json();
                result.style.display = 'block';
                if (res.ok) {
                    result.innerHTML = '<div style="color:#00ff88;padding:15px;background:#0a3d0a;border-radius:8px">✅ ' + json.message + '</div>';
                    this.reset();
                } else {
                    result.innerHTML = '<div style="color:#ff6b6b;padding:15px;background:#3d0a0a;border-radius:8px">❌ ' + json.error + '</div>';
                }
            } catch (err) {
                result.style.display = 'block';
                result.innerHTML = '<div style="color:#ff6b6b;padding:15px;background:#3d0a0a;border-radius:8px">❌ Error de conexión</div>';
            }
            btn.disabled = false;
            btn.textContent = '✉️ Enviar Mensaje';
        });
    </script>

    <!-- Footer -->
    <footer class="footer">
        <div class="footer-grid">
            <div class="footer-col">
                <h4>Vicviewer®</h4>
                <p style="color:#666;font-size:14px;line-height:1.8">La solución de escritorio remoto profesional diseñada para empresas que valoran la seguridad y el rendimiento.</p>
            </div>
            <div class="footer-col">
                <h4>Producto</h4>
                <a href="#features">Características</a>
                <a href="#pricing">Precios</a>
                <a href="#how-it-works">Cómo Funciona</a>
                <a href="/download">Descargar</a>
            </div>
            <div class="footer-col">
                <h4>Empresa</h4>
                <a href="#">Sobre Nosotros</a>
                <a href="#">Blog</a>
                <a href="#">Contacto</a>
                <a href="#">Partners</a>
            </div>
            <div class="footer-col">
                <h4>Legal</h4>
                <a href="#">Términos de Servicio</a>
                <a href="#">Política de Privacidad</a>
                <a href="#">Cookies</a>
            </div>
        </div>
        <div class="footer-bottom">
            <p>&copy; ${new Date().getFullYear()} Vicviewer®. Todos los derechos reservados.</p>
            <div style="margin-top:15px">
                <a href="/admin">Administración</a>
            </div>
        </div>
    </footer>
</body>
</html>`;

// Página principal - detectar subdominio o mostrar landing
app.get('/', (req, res) => {
    // Si hay un usuario de subdominio, mostrar landing personalizada
    if (req.subdomainUser) {
        const user = req.subdomainUser;
        return res.send(`<!DOCTYPE html><html><head>
            <title>${user.company_name || user.name} - Soporte Remoto</title>
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <link rel="icon" type="image/svg+xml" href="/favicon.svg">
            ${subdomainLandingCSS}
        </head><body>
            <div class="hero">
                <div class="logo">🖥️</div>
                <div class="company-name">${user.company_name || user.name}</div>
                <div class="tagline">Sistema de Soporte Remoto Seguro</div>
                
                <div class="code-display">
                    <div class="code-label">Tu código de soporte</div>
                    <div class="code">${user.company_code}</div>
                </div>
                
                <div class="buttons">
                    <a href="/api/download/Vicviewer${user.company_code}.exe" class="btn btn-primary">
                        📥 Descargar Software
                    </a>
                    <a href="/login" class="btn btn-secondary">
                        👤 Acceder al Panel
                    </a>
                </div>
                
                <div class="features">
                    <div class="feature">
                        <div class="feature-icon">🔒</div>
                        <h3>Conexión Segura</h3>
                        <p>Cifrado de extremo a extremo para proteger tu información</p>
                    </div>
                    <div class="feature">
                        <div class="feature-icon">⚡</div>
                        <h3>Ultra Rápido</h3>
                        <p>Tecnología WebRTC para la menor latencia posible</p>
                    </div>
                    <div class="feature">
                        <div class="feature-icon">🛠️</div>
                        <h3>Fácil de Usar</h3>
                        <p>Sin instalación compleja, ejecuta y comparte tu código</p>
                    </div>
                </div>
            </div>
            <div class="footer">
                Powered by Vicviewer® &copy; ${new Date().getFullYear()}
            </div>
        </body></html>`);
    }
    
    // Si no hay subdominio, mostrar landing principal
    res.send(mainLandingHTML);
});

// ============== ENDPOINT CONTACTO ==============
app.post('/api/contact', express.json(), async (req, res) => {
    try {
        const { name, email, subject, message } = req.body;
        
        if (!name || !email || !subject || !message) {
            return res.status(400).json({ error: 'Todos los campos son requeridos' });
        }
        
        // Validar email básico
        const emailRegex = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
        if (!emailRegex.test(email)) {
            return res.status(400).json({ error: 'Email no válido' });
        }
        
        // Usar las credenciales SMTP del admin
        const transporter = createTransporter();
        const fromName = getConfig('smtp_from_name');
        const fromEmail = getConfig('smtp_from_email');
        const adminEmail = fromEmail; // El email del remitente SMTP es el admin
        
        // Enviar email al admin con el mensaje de contacto
        await transporter.sendMail({
            from: `"${fromName}" <${fromEmail}>`,
            to: adminEmail,
            replyTo: email,
            subject: `📬 [Contacto Web] ${subject}`,
            html: `
            <div style="font-family: 'Segoe UI', Arial; background: #1a1a2e; color: #eee; padding: 30px; max-width: 600px; margin: auto; border-radius: 12px;">
                <div style="text-align: center; margin-bottom: 25px;">
                    <div style="font-size: 28px; font-weight: bold; color: #00d4ff;">📬 Nuevo Mensaje de Contacto</div>
                </div>
                <div style="background: #0f3460; padding: 20px; border-radius: 10px; margin-bottom: 20px;">
                    <p style="margin: 5px 0;"><strong style="color: #00d4ff;">Nombre:</strong> ${name}</p>
                    <p style="margin: 5px 0;"><strong style="color: #00d4ff;">Email:</strong> <a href="mailto:${email}" style="color: #00d4ff;">${email}</a></p>
                    <p style="margin: 5px 0;"><strong style="color: #00d4ff;">Asunto:</strong> ${subject}</p>
                </div>
                <div style="background: #16213e; padding: 20px; border-radius: 10px; border-left: 4px solid #00d4ff;">
                    <p style="color: #888; margin-bottom: 10px; font-size: 12px; text-transform: uppercase;">Mensaje:</p>
                    <p style="white-space: pre-wrap; line-height: 1.6;">${message.replace(/</g, '&lt;').replace(/>/g, '&gt;')}</p>
                </div>
                <p style="color: #666; font-size: 12px; margin-top: 20px; text-align: center;">
                    Este mensaje fue enviado desde el formulario de contacto de Vicviewer®
                </p>
            </div>`
        });
        
        log('info', `📬 Mensaje de contacto recibido de ${email}`);
        res.json({ success: true, message: 'Mensaje enviado correctamente. Te responderemos pronto.' });
        
    } catch (error) {
        log('error', `❌ Error enviando mensaje de contacto: ${error.message}`);
        res.status(500).json({ error: 'Error al enviar el mensaje. Inténtalo más tarde.' });
    }
});

// Health check
app.get('/healthz', (req, res) => res.send('ok'));
app.get('/health', (req, res) => {
    res.json({ 
        status: 'ok', 
        activeSessions: sessions.size,
        ttlMs: SESSION_TTL,
        uptime: process.uptime()
    });
});

// Favicon - Icono Vicviewer® (SVG inline)
const faviconSvg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" rx="10" fill="#00d4ff"/>
<rect x="10" y="10" width="80" height="55" rx="8" fill="white"/>
<text x="50" y="45" text-anchor="middle" font-family="Arial,sans-serif" font-size="16" font-weight="bold" fill="#1a237e">Vicviewer</text>
<rect x="25" y="72" width="50" height="12" rx="6" fill="white"/>
</svg>`;

app.get('/favicon.ico', (req, res) => {
    res.set('Content-Type', 'image/svg+xml');
    res.send(faviconSvg);
});

app.get('/favicon.svg', (req, res) => {
    res.set('Content-Type', 'image/svg+xml');
    res.send(faviconSvg);
});

// Crear sesión
app.post('/v1/sessions', async (req, res) => {
    const { code: requestedCode, clientId, codeLength = 9 } = req.body;
    
    const code = requestedCode || Array.from({length: codeLength}, () => Math.floor(Math.random() * 10)).join('');
    const now = Date.now();
    
    sessions.set(code, {
        code,
        clientId,
        offer: null,
        answer: null,
        createdAt: now,
        expiresAt: now + SESSION_TTL
    });

    let emailSent = false;
    
    // Buscar usuario y enviar email
    if (clientId) {
        const user = db.prepare('SELECT * FROM users WHERE company_code = ? AND status = ?').get(clientId, 'active');
        if (user) {
            try {
                await sendAccessCode(user.email, user.name, code);
                emailSent = true;
                db.prepare('UPDATE users SET last_connection = ?, updated_at = ? WHERE company_code = ?')
                  .run(new Date().toISOString(), new Date().toISOString(), clientId);
                log('info', `📧 Email enviado a ${user.email} con código ${code}`);
            } catch (err) {
                log('error', `Error enviando email: ${err.message}`);
            }
        }
    }

    log('info', `✅ Sesión creada: ${code} (clientId: ${clientId || 'ninguno'})`);
    res.status(201).json({ code, expiresInMillis: SESSION_TTL, emailSent });
});

// ============== ENDPOINT /register (compatibilidad con Vicviewer® C++) ==============
// Soporta: códigos dinámicos (por defecto) y códigos fijos (servicios instalados)
app.post('/register', async (req, res) => {
    const { code: requestedCode, clientId, offer, iceCandidates, iceServers, isService, deviceName } = req.body;
    const ipAddress = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
    const userAgent = req.headers['user-agent'] || 'unknown';
    
    log('info', `📥 /register - clientId: ${clientId || 'ninguno'}, code: ${requestedCode || 'auto'}, service: ${isService || false}`);
    
    let code = requestedCode;
    let isFixedCode = false;
    let realClientId = clientId;  // Por defecto usar el que viene (puede ser company_code)
    
    // Si es servicio y tiene código solicitado, verificar/registrar como código fijo
    if (isService && requestedCode && clientId) {
        // Resolver company_code a client_id real para la FK
        const user = db.prepare('SELECT id, client_id FROM users WHERE company_code = ?').get(clientId);
        if (user) {
            realClientId = user.client_id;
            log('info', `🔍 Resuelto company_code ${clientId} -> client_id ${realClientId}`);
        } else {
            log('warn', `⚠️ No se encontró usuario con company_code: ${clientId}`);
        }
        
        // Verificar si el código fijo ya existe
        const existingDevice = db.prepare('SELECT * FROM devices WHERE device_code = ?').get(requestedCode);
        
        if (existingDevice) {
            // Si existe pero es de otro usuario, error (comparar con client_id real)
            if (existingDevice.client_id != realClientId) {
                log('warn', `❌ Código fijo ${requestedCode} ya en uso por otro usuario (device.client_id=${existingDevice.client_id}, realClientId=${realClientId})`);
                return res.status(409).json({ error: 'code_in_use', message: 'Este código ya está en uso por otro usuario' });
            }
            // Si es del mismo usuario, actualizar
            db.prepare(`UPDATE devices SET 
                is_online = 1, 
                ip_address = ?, 
                offer_sdp = ?, 
                ice_candidates = ?, 
                ice_servers = ?,
                last_seen = ?,
                updated_at = ?
                WHERE device_code = ?`).run(
                ipAddress,
                offer?.sdp || null,
                JSON.stringify(iceCandidates || []),
                JSON.stringify(iceServers || []),
                new Date().toISOString(),
                new Date().toISOString(),
                requestedCode
            );
            isFixedCode = true;
            log('info', `🔄 Dispositivo actualizado: ${requestedCode} (online=1, offer guardado)`);
        } else {
            // Registrar nuevo dispositivo con código fijo usando client_id real
            db.prepare(`INSERT INTO devices (client_id, device_code, device_name, ip_address, is_online, is_service, offer_sdp, ice_candidates, ice_servers, last_seen)
                VALUES (?, ?, ?, ?, 1, 1, ?, ?, ?, ?)`).run(
                realClientId,
                requestedCode,
                deviceName || `Equipo ${requestedCode}`,
                ipAddress,
                offer?.sdp || null,
                JSON.stringify(iceCandidates || []),
                JSON.stringify(iceServers || []),
                new Date().toISOString()
            );
            isFixedCode = true;
            log('info', `✨ Nuevo dispositivo registrado: ${requestedCode} con client_id=${realClientId}`);
        }
    }
    
    // Generar código si no se proporcionó
    if (!code) {
        code = Array.from({length: 6}, () => Math.random().toString(36).charAt(2).toUpperCase()).join('');
    }
    
    const now = Date.now();
    
    // Guardar sesión en memoria (para códigos dinámicos o como cache de fijos)
    sessions.set(code, {
        code,
        clientId,
        offer: offer?.sdp || null,
        answer: null,
        iceCandidates: iceCandidates || [],
        iceServers: iceServers || [],
        isFixedCode,
        createdAt: now,
        expiresAt: isFixedCode ? now + (365 * 24 * 60 * 60 * 1000) : now + SESSION_TTL // Fijos: 1 año
    });

    let emailSent = false;
    
    if (clientId) {
        // Registrar en historial (solo si no es un refresh de servicio)
        if (!isFixedCode || !sessions.has(code)) {
            db.prepare('INSERT INTO connection_history (client_id, access_code, ip_address, user_agent, action) VALUES (?, ?, ?, ?, ?)')
              .run(clientId, code, ipAddress, userAgent, isService ? 'service_started' : 'host_started');
        }
        
        const user = db.prepare('SELECT * FROM users WHERE company_code = ? AND status = ?').get(clientId, 'active');
        if (user) {
            // Solo enviar email si NO es código fijo (servicios no envían email cada vez)
            if (!isFixedCode) {
                try {
                    await sendAccessCode(user.email, user.name, code);
                    emailSent = true;
                    log('info', `📧 Email enviado a ${user.email} con código ${code}`);
                } catch (err) {
                    log('error', `❌ Error enviando email: ${err.message}`);
                }
            }
            db.prepare('UPDATE users SET last_connection = ?, updated_at = ? WHERE company_code = ?')
              .run(new Date().toISOString(), new Date().toISOString(), clientId);
        } else {
            log('warn', `⚠️ Usuario '${clientId}' no encontrado`);
        }
    }

    // Determinar modo (paid vs free)
    let mode = 'paid';
    let maxDurationMs = null;
    const { companyCode, diskSerial } = req.body;
    
    if (diskSerial) {
        const freeModeStatus = checkFreeModeStatus(diskSerial, companyCode);
        if (!freeModeStatus.isPaid) {
            if (!freeModeStatus.allowed) {
                return res.status(429).json({
                    error: 'cooldown',
                    message: freeModeStatus.message,
                    waitMinutes: freeModeStatus.waitMinutes
                });
            }
            mode = 'free';
            maxDurationMs = FREE_MODE_SESSION_DURATION;
            startFreeSession(diskSerial, companyCode);
        }
    }

    log('info', `✅ Host registrado: ${code} (fijo: ${isFixedCode}, modo: ${mode})`);
    res.status(201).json({
        code,
        expiresInMillis: isFixedCode ? null : SESSION_TTL,
        emailSent,
        isFixedCode,
        success: true,
        mode,
        maxDurationMs,
        maxDurationMinutes: maxDurationMs ? Math.floor(maxDurationMs / 60000) : null
    });
});

// Guardar/obtener offer
app.put('/v1/sessions/:code/offer', (req, res) => {
    const session = sessions.get(req.params.code);
    if (!session) return res.status(404).json({ error: 'session not found' });
    session.offer = req.body.sdp;
    res.json({ sessionId: session.code, expiresInMillis: session.expiresAt - Date.now() });
});

app.get('/v1/sessions/:code/offer', (req, res) => {
    const session = sessions.get(req.params.code);
    if (!session?.offer) return res.status(404).json({ error: 'offer not found' });
    
    // Registrar que alguien solicitó conectarse
    if (session.clientId) {
        const ipAddress = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
        const userAgent = req.headers['user-agent'] || 'unknown';
        db.prepare('INSERT INTO connection_history (client_id, access_code, ip_address, user_agent, action) VALUES (?, ?, ?, ?, ?)')
          .run(session.clientId, req.params.code, ipAddress, userAgent, 'viewer_connected');
    }
    
    res.json({ sdp: session.offer });
});

// Guardar/obtener answer
app.put('/v1/sessions/:code/answer', (req, res) => {
    const session = sessions.get(req.params.code);
    if (!session) return res.status(404).json({ error: 'session not found' });
    session.answer = req.body.sdp;
    res.sendStatus(204);
});

// ============== ENDPOINT /answer (para Viewer C++) ==============
// El viewer envía: POST /answer con { code, answer: {type, sdp}, iceCandidates }
app.post('/answer', (req, res) => {
    const { code, answer, iceCandidates } = req.body;
    
    log('info', `📤 /answer recibido para código: ${code}`);
    
    if (!code) {
        return res.status(400).json({ error: 'code required' });
    }
    
    const session = sessions.get(code);
    if (!session) {
        log('warn', `/answer - sesión no encontrada: ${code}`);
        return res.status(404).json({ error: 'session not found' });
    }
    
    // Guardar answer completo
    session.answer = answer?.sdp || null;
    session.viewerAnswer = answer;
    session.viewerCandidates = iceCandidates || [];
    
    log('info', `✅ /answer guardado para sesión: ${code}`);
    res.json({ success: true });
});

// GET /answer?code=XXX (para que el HOST obtenga el answer del viewer)
app.get('/answer', (req, res) => {
    const code = req.query.code;
    
    if (!code) {
        return res.status(400).json({ error: 'code required' });
    }
    
    const session = sessions.get(code);
    if (!session) {
        return res.status(404).json({ error: 'session not found' });
    }
    
    if (!session.viewerAnswer) {
        return res.status(404).json({ error: 'answer not ready' });
    }
    
    log('debug', `📥 /answer GET - Host obteniendo answer para: ${code}`);
    
    res.json({
        answer: session.viewerAnswer,
        iceCandidates: session.viewerCandidates || []
    });
});

app.get('/v1/sessions/:code/answer', (req, res) => {
    const session = sessions.get(req.params.code);
    if (!session?.answer) return res.status(404).json({ error: 'answer not found' });
    res.json({ sdp: session.answer });
});

// ============== ENDPOINT /resolve (para Viewer C++) ==============
// El viewer busca sesiones con GET /resolve?code=XXXXX
// Busca primero en memoria, luego en dispositivos persistentes
app.get('/resolve', (req, res) => {
    const code = req.query.code;
    
    if (!code) {
        log('warn', `/resolve - código no proporcionado`);
        return res.status(400).json({ error: 'code parameter required' });
    }
    
    log('info', `🔍 /resolve buscando código: ${code}`);
    
    // Primero buscar en memoria (sesiones activas)
    let session = sessions.get(code);
    
    // Si no está en memoria, buscar en dispositivos persistentes
    if (!session) {
        const device = db.prepare('SELECT * FROM devices WHERE device_code = ? AND is_online = 1').get(code);
        if (device) {
            log('info', `📱 Dispositivo persistente encontrado: ${code}`);
            // Recrear sesión desde dispositivo
            session = {
                code: device.device_code,
                clientId: device.client_id,
                offer: device.offer_sdp,
                iceCandidates: JSON.parse(device.ice_candidates || '[]'),
                iceServers: JSON.parse(device.ice_servers || '[]'),
                isFixedCode: true
            };
            // Cachear en memoria
            sessions.set(code, session);
        }
    }
    
    if (!session) {
        log('warn', `/resolve - sesión no encontrada: ${code}`);
        log('debug', `Sesiones activas: ${Array.from(sessions.keys()).join(', ') || 'ninguna'}`);
        return res.status(404).json({ error: 'session not found' });
    }
    
    if (!session.offer) {
        log('warn', `/resolve - sesión encontrada pero sin offer: ${code}`);
        return res.status(404).json({ error: 'offer not ready' });
    }
    
    // Registrar que viewer está buscando conectarse
    if (session.clientId) {
        const ipAddress = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
        const userAgent = req.headers['user-agent'] || 'unknown';
        db.prepare('INSERT INTO connection_history (client_id, access_code, ip_address, user_agent, action) VALUES (?, ?, ?, ?, ?)')
          .run(session.clientId, code, ipAddress, userAgent, 'viewer_connecting');
    }
    
    log('info', `✅ /resolve - sesión encontrada: ${code} (fijo: ${session.isFixedCode || false})`);
    
    // Devolver en el formato que espera el viewer C++
    res.json({
        code: session.code,
        offer: {
            type: 'offer',
            sdp: session.offer
        },
        iceCandidates: session.iceCandidates || [],
        iceServers: session.iceServers || []
    });
});

// Debug sessions
app.get('/debug/sessions', (req, res) => {
    const list = Array.from(sessions.values()).map(s => ({
        code: s.code,
        clientId: s.clientId,
        hasOffer: !!s.offer,
        hasAnswer: !!s.answer,
        expiresIn: Math.round((s.expiresAt - Date.now()) / 1000) + 's'
    }));
    res.json({ count: list.length, sessions: list });
});

// ============== API CÓDIGOS ==============
// Generar código único disponible (el EXE puede solicitar uno antes de registrarse)
app.get('/api/generate-code', (req, res) => {
    const length = parseInt(req.query.length) || 6;
    let code;
    let attempts = 0;
    const maxAttempts = 100;
    
    do {
        code = Array.from({length}, () => 
            'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[Math.floor(Math.random() * 32)]
        ).join('');
        attempts++;
    } while (
        (sessions.has(code) || db.prepare('SELECT 1 FROM devices WHERE device_code = ?').get(code)) 
        && attempts < maxAttempts
    );
    
    if (attempts >= maxAttempts) {
        return res.status(500).json({ error: 'No se pudo generar código único' });
    }
    
    log('info', `🎲 Código generado: ${code}`);
    res.json({ code, available: true });
});

// Verificar si un código específico está disponible
app.get('/api/check-code', (req, res) => {
    const { code } = req.query;
    
    if (!code) {
        return res.status(400).json({ error: 'code required' });
    }
    
    // Verificar en sesiones activas y dispositivos registrados
    const inSession = sessions.has(code);
    const inDevices = db.prepare('SELECT client_id FROM devices WHERE device_code = ?').get(code);
    
    const available = !inSession && !inDevices;
    const owner = inDevices?.client_id || null;
    
    log('debug', `🔍 Check code ${code}: ${available ? 'disponible' : 'en uso'}`);
    res.json({ code, available, owner });
});

// ============== API USUARIOS ==============
app.get('/api/users', (req, res) => {
    const users = db.prepare('SELECT * FROM users ORDER BY created_at DESC').all();
    res.json(users);
});

app.post('/api/users', (req, res) => {
    const { client_id, name, email, phone = '', status = 'active' } = req.body;
    try {
        const result = db.prepare(`
            INSERT INTO users (client_id, name, email, phone, status, activation_date)
            VALUES (?, ?, ?, ?, ?, ?)
        `).run(client_id, name, email, phone, status, new Date().toISOString());
        res.status(201).json({ id: result.lastInsertRowid, client_id, name, email });
    } catch (err) {
        res.status(400).json({ error: err.message });
    }
});

app.put('/api/users/:id', (req, res) => {
    const { client_id, name, email, phone, status } = req.body;
    db.prepare(`
        UPDATE users SET client_id=?, name=?, email=?, phone=?, status=?, updated_at=?
        WHERE id=?
    `).run(client_id, name, email, phone, status, new Date().toISOString(), req.params.id);
    res.json({ success: true });
});

app.delete('/api/users/:id', (req, res) => {
    db.prepare('DELETE FROM users WHERE id = ?').run(req.params.id);
    res.json({ success: true });
});

// ============== ESTILOS CSS ==============
const adminCSS = `
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
<style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: 'Segoe UI', sans-serif; background: #0f0f1a; color: #eee; min-height: 100vh; }
    .container { max-width: 1200px; margin: 0 auto; padding: 20px; }
    .header { display: flex; justify-content: space-between; align-items: center; padding: 20px 0; border-bottom: 1px solid #333; margin-bottom: 30px; }
    .logo { font-size: 24px; font-weight: bold; color: #00d4ff; display: flex; align-items: center; gap: 10px; }
    .logo img { width: 32px; height: 32px; }
    .nav a { color: #888; text-decoration: none; margin-left: 20px; } .nav a:hover { color: #fff; }
    .nav .logout { color: #ff6b6b; }
    .card { background: #1a1a2e; border-radius: 12px; padding: 25px; margin-bottom: 20px; }
    .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; margin-bottom: 30px; }
    .stat { background: linear-gradient(135deg, #16213e 0%, #1a1a2e 100%); border-radius: 10px; padding: 20px; text-align: center; }
    .stat-value { font-size: 36px; font-weight: bold; color: #00d4ff; }
    .stat-label { color: #888; font-size: 14px; margin-top: 5px; }
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 12px 15px; text-align: left; border-bottom: 1px solid #333; }
    th { color: #888; font-weight: 500; }
    tr:hover { background: #16213e; }
    .btn { padding: 8px 16px; border: none; border-radius: 6px; cursor: pointer; font-size: 14px; text-decoration: none; display: inline-block; }
    .btn-primary { background: #00d4ff; color: #000; } .btn-primary:hover { background: #00b8e6; }
    .btn-danger { background: #ff4757; color: #fff; } .btn-danger:hover { background: #ff3344; }
    .btn-success { background: #10b981; color: #fff; } .btn-success:hover { background: #059669; }
    .btn-sm { padding: 4px 10px; font-size: 12px; }
    .status { padding: 4px 10px; border-radius: 20px; font-size: 12px; }
    .status-active { background: #00d4ff22; color: #00d4ff; }
    .status-disabled { background: #ff475722; color: #ff4757; }
    .form-group { margin-bottom: 15px; }
    .form-group label { display: block; margin-bottom: 5px; color: #888; }
    .form-group input, .form-group select { width: 100%; padding: 10px; border: 1px solid #333; border-radius: 6px; background: #16213e; color: #eee; }
    .form-row { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; }
    code { background: #16213e; padding: 2px 8px; border-radius: 4px; font-family: Consolas; }
    .page-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
    .empty { text-align: center; padding: 50px; color: #666; }
    .alert { padding: 15px; border-radius: 8px; margin-bottom: 20px; }
    .alert-success { background: #10b98122; border: 1px solid #10b981; color: #10b981; }
    .alert-error { background: #ff475722; border: 1px solid #ff4757; color: #ff4757; }
    .login-container { display: flex; justify-content: center; align-items: center; min-height: 100vh; }
    .login-box { background: #1a1a2e; padding: 40px; border-radius: 16px; width: 100%; max-width: 400px; }
    .login-box h1 { text-align: center; margin-bottom: 30px; color: #00d4ff; }
    .login-box .btn { width: 100%; padding: 12px; }
    .tabs { display: flex; border-bottom: 1px solid #333; margin-bottom: 20px; }
    .tabs a { padding: 10px 20px; color: #888; text-decoration: none; border-bottom: 2px solid transparent; cursor: pointer; }
    .tabs a.active, .tabs a:hover { color: #00d4ff; border-bottom-color: #00d4ff; }
</style>`;

const adminHeader = (currentPath = '') => `
<div class="header">
    <div class="logo"><img src="/favicon.svg" alt="Vicviewer®"> Vicviewer® Admin</div>
    <nav class="nav">
        <a href="/admin" ${currentPath === 'dashboard' ? 'style="color:#00d4ff"' : ''}>Dashboard</a>
        <a href="/admin/users" ${currentPath === 'users' ? 'style="color:#00d4ff"' : ''}>Usuarios</a>
        <a href="/admin/devices" ${currentPath === 'devices' ? 'style="color:#00d4ff"' : ''}>Dispositivos</a>
        <a href="/admin/history" ${currentPath === 'history' ? 'style="color:#00d4ff"' : ''}>Historial</a>
        <a href="/admin/config" ${currentPath === 'config' ? 'style="color:#00d4ff"' : ''}>Configuración</a>
        <a href="/admin/logout" class="logout">🚪 Salir</a>
    </nav>
</div>`;

// ============== LOGIN ==============
app.get('/admin/login', (req, res) => {
    const error = req.query.error;
    res.send(`<!DOCTYPE html><html><head><title>Login - Vicviewer® Admin</title>${adminCSS}</head><body>
    <div class="login-container">
        <div class="login-box">
            <h1>🖥️ Vicviewer®</h1>
            ${error ? '<div class="alert alert-error">Usuario o contraseña incorrectos</div>' : ''}
            <form method="POST" action="/admin/login">
                <div class="form-group">
                    <label>Usuario</label>
                    <input name="username" type="text" required autofocus placeholder="admin">
                </div>
                <div class="form-group">
                    <label>Contraseña</label>
                    <input name="password" type="password" required placeholder="••••••••">
                </div>
                <button type="submit" class="btn btn-primary">🔐 Iniciar Sesión</button>
            </form>
        </div>
    </div></body></html>`);
});

app.post('/admin/login', (req, res) => {
    const { username, password } = req.body;
    const adminUser = getConfig('admin_user');
    const adminPassHash = getConfig('admin_pass_hash');
    const inputHash = crypto.createHash('sha256').update(password).digest('hex');
    
    if (username === adminUser && inputHash === adminPassHash) {
        const token = createAdminSession();
        res.cookie('admin_token', token, { 
            httpOnly: true, 
            maxAge: ADMIN_SESSION_TTL,
            sameSite: 'strict'
        });
        return res.redirect('/admin');
    }
    
    res.redirect('/admin/login?error=1');
});

app.get('/admin/logout', (req, res) => {
    const token = req.cookies?.admin_token;
    if (token) {
        db.prepare('DELETE FROM admin_sessions WHERE token = ?').run(token);
    }
    res.clearCookie('admin_token');
    res.redirect('/admin/login');
});

// ============== PANEL ADMIN (protegido) ==============
app.get('/admin', authMiddleware, (req, res) => {
    const total = db.prepare('SELECT COUNT(*) as c FROM users').get().c;
    const active = db.prepare("SELECT COUNT(*) as c FROM users WHERE status='active'").get().c;
    const sessions_count = sessions.size;
    const devicesTotal = db.prepare('SELECT COUNT(*) as c FROM devices').get().c;
    const devicesOnline = db.prepare('SELECT COUNT(*) as c FROM devices WHERE is_online = 1').get().c;
    
    res.send(`<!DOCTYPE html><html><head><title>Admin - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('dashboard')}
        <div class="stats">
            <div class="stat"><div class="stat-value">${total}</div><div class="stat-label">Total Usuarios</div></div>
            <div class="stat"><div class="stat-value">${active}</div><div class="stat-label">Activos</div></div>
            <div class="stat"><div class="stat-value">${sessions_count}</div><div class="stat-label">Sesiones Activas</div></div>
            <div class="stat" style="background:linear-gradient(135deg, #064e3b 0%, #1a1a2e 100%)"><div class="stat-value">${devicesOnline}/${devicesTotal}</div><div class="stat-label">💻 Dispositivos Online</div></div>
        </div>
        <div class="card">
            <h3 style="margin-bottom:15px">Acciones Rápidas</h3>
            <a href="/admin/users/new" class="btn btn-primary">➕ Nuevo Usuario</a>
            <a href="/admin/users" class="btn" style="background:#333;color:#fff;margin-left:10px">👥 Ver Usuarios</a>
            <a href="/admin/devices" class="btn" style="background:#333;color:#fff;margin-left:10px">💻 Dispositivos</a>
            <a href="/admin/config" class="btn" style="background:#333;color:#fff;margin-left:10px">⚙️ Configuración</a>
        </div>
    </div></body></html>`);
});

app.get('/admin/users', authMiddleware, (req, res) => {
    const users = db.prepare('SELECT * FROM users ORDER BY created_at DESC').all();
    const rows = users.map(u => `
        <tr>
            <td><code>${u.client_id}</code></td>
            <td>
                <strong>${u.name}</strong>
                ${u.company_name ? `<br><small style="color:#888">${u.company_name}</small>` : ''}
            </td>
            <td>${u.email}${!u.email_verified ? '<br><small style="color:#ff6b6b">⚠️ No verificado</small>' : ''}</td>
            <td>
                ${u.company_code ? `<code style="background:#0f3460;padding:3px 8px;font-size:14px;letter-spacing:2px">${u.company_code}</code>` : '<span style="color:#666">-</span>'}
            </td>
            <td>
                ${u.can_view_history ? '📜' : ''}${u.can_view_devices ? '💻' : ''}${u.has_subdomain ? '🌐' : ''}${u.custom_banner ? '🖼️' : ''}
                ${!u.can_view_history && !u.can_view_devices && !u.has_subdomain && !u.custom_banner ? '<span style="color:#666">-</span>' : ''}
            </td>
            <td><span class="status status-${u.status === 'active' ? 'active' : 'disabled'}">${u.status === 'active' ? '✅ Activo' : u.status === 'pending' ? '⏳ Pendiente' : '🚫 Deshabilitado'}</span></td>
            <td>
                <a href="/admin/users/${u.id}/edit" class="btn btn-sm" style="background:#333;color:#fff">✏️</a>
                <button onclick="del(${u.id},'${u.name.replace(/'/g, "\\'")}')" class="btn btn-sm btn-danger">🗑️</button>
            </td>
        </tr>
    `).join('');
    
    res.send(`<!DOCTYPE html><html><head><title>Usuarios - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('users')}
        <div class="page-header">
            <h2>👥 Usuarios</h2>
            <a href="/admin/users/new" class="btn btn-primary">➕ Nuevo</a>
        </div>
        <div class="card">
            ${users.length ? `<table>
                <thead><tr><th>ID Cliente</th><th>Nombre</th><th>Email</th><th>Código Empresa</th><th>Permisos</th><th>Estado</th><th>Acciones</th></tr></thead>
                <tbody>${rows}</tbody>
            </table>` : '<div class="empty">No hay usuarios. <a href="/admin/users/new">Crear uno</a></div>'}
        </div>
    </div>
    <script>
        function del(id, name) {
            if(confirm('¿Eliminar a '+name+'?')) {
                fetch('/api/users/'+id, {method:'DELETE'}).then(() => location.reload());
            }
        }
    </script>
    </body></html>`);
});

// ============== HISTORIAL DE CONEXIONES ==============
app.get('/admin/history', authMiddleware, (req, res) => {
    const clientId = req.query.client_id || '';
    const page = parseInt(req.query.page) || 1;
    const limit = 50;
    const offset = (page - 1) * limit;
    
    let query = `SELECT h.*, u.name as user_name FROM connection_history h 
                 LEFT JOIN users u ON h.client_id = u.client_id`;
    let countQuery = 'SELECT COUNT(*) as total FROM connection_history h';
    const params = [];
    
    if (clientId) {
        query += ' WHERE h.client_id = ?';
        countQuery += ' WHERE h.client_id = ?';
        params.push(clientId);
    }
    
    query += ' ORDER BY h.created_at DESC LIMIT ? OFFSET ?';
    
    const history = db.prepare(query).all(...params, limit, offset);
    const totalCount = db.prepare(countQuery).get(...params).total;
    const totalPages = Math.ceil(totalCount / limit);
    
    // Obtener lista de usuarios para filtro
    const users = db.prepare('SELECT client_id, name FROM users ORDER BY name').all();
    
    const actionLabels = {
        'host_started': '🟢 Host Iniciado',
        'viewer_connected': '👁️ Viewer Conectado',
        'session_ended': '🔴 Sesión Terminada'
    };
    
    const rows = history.map(h => `
        <tr>
            <td>${new Date(h.created_at).toLocaleString()}</td>
            <td><code>${h.client_id}</code> ${h.user_name ? `<small>(${h.user_name})</small>` : ''}</td>
            <td><code style="background:#0f3460;padding:3px 8px;border-radius:4px">${h.access_code}</code></td>
            <td>${actionLabels[h.action] || h.action}</td>
            <td><small>${h.ip_address || '-'}</small></td>
        </tr>
    `).join('');
    
    const userOptions = users.map(u => 
        `<option value="${u.client_id}" ${u.client_id === clientId ? 'selected' : ''}>${u.name} (${u.client_id})</option>`
    ).join('');
    
    const pagination = totalPages > 1 ? `
        <div style="margin-top:20px;text-align:center">
            ${page > 1 ? `<a href="?client_id=${clientId}&page=${page-1}" class="btn btn-sm">← Anterior</a>` : ''}
            <span style="margin:0 15px;color:#888">Página ${page} de ${totalPages} (${totalCount} registros)</span>
            ${page < totalPages ? `<a href="?client_id=${clientId}&page=${page+1}" class="btn btn-sm">Siguiente →</a>` : ''}
        </div>
    ` : '';
    
    res.send(`<!DOCTYPE html><html><head><title>Historial - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('history')}
        <div class="page-header">
            <h2>📜 Historial de Conexiones</h2>
            <form method="GET" style="display:flex;gap:10px">
                <select name="client_id" style="background:#1a1a2e;color:#fff;border:1px solid #333;padding:8px 12px;border-radius:6px">
                    <option value="">-- Todos los usuarios --</option>
                    ${userOptions}
                </select>
                <button type="submit" class="btn btn-primary">Filtrar</button>
            </form>
        </div>
        <div class="card">
            ${history.length ? `<table>
                <thead><tr><th>Fecha/Hora</th><th>Usuario</th><th>Código</th><th>Acción</th><th>IP</th></tr></thead>
                <tbody>${rows}</tbody>
            </table>${pagination}` : '<div class="empty">No hay registros de conexiones.</div>'}
        </div>
    </div>
    </body></html>`);
});

// ============== DISPOSITIVOS (PCs con servicio instalado) ==============
app.get('/admin/devices', authMiddleware, (req, res) => {
    const clientId = req.query.client_id || '';
    
    let query = `SELECT d.*, u.name as user_name FROM devices d 
                 LEFT JOIN users u ON d.client_id = u.client_id`;
    const params = [];
    
    if (clientId) {
        query += ' WHERE d.client_id = ?';
        params.push(clientId);
    }
    query += ' ORDER BY d.is_online DESC, d.last_seen DESC';
    
    const devices = db.prepare(query).all(...params);
    const users = db.prepare('SELECT client_id, name FROM users ORDER BY name').all();
    
    // Contar online/offline
    const onlineCount = devices.filter(d => d.is_online).length;
    const offlineCount = devices.length - onlineCount;
    
    const rows = devices.map(d => `
        <tr>
            <td>
                <span style="display:inline-block;width:10px;height:10px;border-radius:50%;background:${d.is_online ? '#10b981' : '#666'};margin-right:8px"></span>
                <code style="background:#0f3460;padding:3px 10px;border-radius:4px;font-size:16px;font-weight:bold">${d.device_code}</code>
            </td>
            <td>${d.device_name || '-'}</td>
            <td><code>${d.client_id}</code> ${d.user_name ? `<small>(${d.user_name})</small>` : ''}</td>
            <td><small>${d.ip_address || '-'}</small></td>
            <td>${d.last_seen ? new Date(d.last_seen).toLocaleString() : '-'}</td>
            <td>
                <span class="status ${d.is_online ? 'status-active' : 'status-disabled'}">
                    ${d.is_online ? '🟢 Online' : '⚫ Offline'}
                </span>
            </td>
            <td>
                <button onclick="delDevice('${d.device_code}')" class="btn btn-danger btn-sm">🗑️</button>
            </td>
        </tr>
    `).join('');
    
    const userOptions = users.map(u => 
        `<option value="${u.client_id}" ${u.client_id === clientId ? 'selected' : ''}>${u.name} (${u.client_id})</option>`
    ).join('');
    
    res.send(`<!DOCTYPE html><html><head><title>Dispositivos - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('devices')}
        <div class="page-header">
            <h2>💻 Dispositivos con Servicio</h2>
            <form method="GET" style="display:flex;gap:10px">
                <select name="client_id" style="background:#1a1a2e;color:#fff;border:1px solid #333;padding:8px 12px;border-radius:6px">
                    <option value="">-- Todos los usuarios --</option>
                    ${userOptions}
                </select>
                <button type="submit" class="btn btn-primary">Filtrar</button>
            </form>
        </div>
        <div class="stats" style="margin-bottom:20px">
            <div class="stat" style="background:linear-gradient(135deg, #064e3b 0%, #1a1a2e 100%)">
                <div class="stat-value">${onlineCount}</div>
                <div class="stat-label">🟢 Online</div>
            </div>
            <div class="stat">
                <div class="stat-value">${offlineCount}</div>
                <div class="stat-label">⚫ Offline</div>
            </div>
            <div class="stat">
                <div class="stat-value">${devices.length}</div>
                <div class="stat-label">📱 Total</div>
            </div>
        </div>
        <div class="card">
            ${devices.length ? `<table>
                <thead><tr><th>Código Fijo</th><th>Nombre</th><th>Usuario</th><th>IP</th><th>Última Actividad</th><th>Estado</th><th>Acciones</th></tr></thead>
                <tbody>${rows}</tbody>
            </table>` : '<div class="empty">No hay dispositivos registrados con servicio.</div>'}
        </div>
        <div style="margin-top:20px;padding:15px;background:#1a1a2e;border-radius:8px;font-size:13px;color:#888">
            <strong>ℹ️ Info:</strong> Los dispositivos aparecen aquí cuando Vicviewer® se instala como servicio con un código fijo.
            El estado se actualiza cada minuto mediante heartbeat. Si no hay heartbeat en 5 minutos, se marca como offline.
        </div>
    </div>
    <script>
        function delDevice(code) {
            if(confirm('¿Eliminar dispositivo con código '+code+'? El servicio deberá reinstalarse.')) {
                fetch('/api/devices/'+code, {method:'DELETE'}).then(() => location.reload());
            }
        }
        // Auto-refresh cada 30 segundos
        setTimeout(() => location.reload(), 30000);
    </script>
    </body></html>`);
});

app.get('/admin/users/new', authMiddleware, (req, res) => {
    const newCode = generateCompanyCode();
    res.send(`<!DOCTYPE html><html><head><title>Nuevo Usuario - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('users')}
        <h2 style="margin-bottom:20px">➕ Nuevo Usuario</h2>
        <div class="card">
            <form method="POST" action="/admin/users/new">
                <div class="form-row">
                    <div class="form-group"><label>ID Cliente *</label><input name="client_id" required placeholder="Ej: 1234"></div>
                    <div class="form-group"><label>Nombre *</label><input name="name" required placeholder="Nombre completo"></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>Email *</label><input name="email" type="email" required placeholder="correo@ejemplo.com"></div>
                    <div class="form-group"><label>Teléfono</label><input name="phone" placeholder="+52 555 123 4567"></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>Empresa</label><input name="company_name" placeholder="Nombre de empresa"></div>
                    <div class="form-group"><label>Contraseña</label><input name="password" type="password" placeholder="Dejar vacío para enviar email de activación"></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>País</label><input name="country" placeholder="Ej: México"></div>
                    <div class="form-group"><label>Estado/Provincia</label><input name="state" placeholder="Ej: Jalisco"></div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>Código de Empresa (4 dígitos)</label>
                        <div style="display:flex;gap:10px">
                            <input name="company_code" id="company_code" value="${newCode}" maxlength="4" style="text-transform:uppercase;font-family:Consolas;font-size:18px;letter-spacing:4px" placeholder="XXXX">
                            <button type="button" onclick="genCode()" class="btn btn-success">🎲 Generar</button>
                        </div>
                    </div>
                    <div class="form-group"><label>Estado</label>
                        <select name="status"><option value="active">✅ Activo</option><option value="pending">⏳ Pendiente</option><option value="disabled">🚫 Deshabilitado</option></select>
                    </div>
                </div>
                <div style="background:#16213e;padding:15px;border-radius:8px;margin:15px 0">
                    <h4 style="margin-bottom:10px">🔐 Permisos del Usuario</h4>
                    <label style="display:flex;align-items:center;gap:10px;margin-bottom:8px;cursor:pointer">
                        <input type="checkbox" name="can_view_history" value="1" checked> 📜 Puede ver historial de conexiones
                    </label>
                    <label style="display:flex;align-items:center;gap:10px;margin-bottom:8px;cursor:pointer">
                        <input type="checkbox" name="can_view_devices" value="1" checked> 💻 Puede ver dispositivos
                    </label>
                    <label style="display:flex;align-items:center;gap:10px;margin-bottom:8px;cursor:pointer">
                        <input type="checkbox" name="has_subdomain" value="1"> 🌐 Tiene subdominio personalizado
                    </label>
                    <label style="display:flex;align-items:center;gap:10px;cursor:pointer">
                        <input type="checkbox" name="custom_banner" value="1"> 🖼️ Puede personalizar banner
                    </label>
                </div>
                <button type="submit" class="btn btn-primary">💾 Crear Usuario</button>
                <a href="/admin/users" class="btn" style="background:#333;color:#fff;margin-left:10px">Cancelar</a>
            </form>
        </div>
    </div>
    <script>
        function genCode() {
            if (!confirm('⚠️ ADVERTENCIA\n\nGenerar código de empresa:\n• Se asignará un nuevo código único\n• Este código identificará la empresa\n\n¿Desea continuar?')) return;
            const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ0123456789';
            let code = '';
            for(let i=0;i<4;i++) code += chars[Math.floor(Math.random()*chars.length)];
            document.getElementById('company_code').value = code;
        }
    </script>
    </body></html>`);
});

app.post('/admin/users/new', authMiddleware, (req, res) => {
    const { client_id, name, email, phone = '', status = 'active', company_name = '', country = '', state = '', company_code = '', password = '' } = req.body;
    const can_view_history = req.body.can_view_history ? 1 : 0;
    const can_view_devices = req.body.can_view_devices ? 1 : 0;
    const has_subdomain = req.body.has_subdomain ? 1 : 0;
    const custom_banner = req.body.custom_banner ? 1 : 0;
    
    try {
        // Verificar que el código de empresa no exista
        if (company_code) {
            const existing = db.prepare('SELECT 1 FROM users WHERE company_code = ?').get(company_code.toUpperCase());
            if (existing) {
                return res.send(`<div style="padding:40px;color:#ff4757">❌ El código de empresa "${company_code}" ya está en uso. <a href="/admin/users/new" style="color:#00d4ff">Volver</a></div>`);
            }
        }
        
        const password_hash = password ? crypto.createHash('sha256').update(password).digest('hex') : null;
        const email_verified = password ? 1 : 0;
        const finalStatus = password ? status : 'pending';
        const verification_token = password ? null : generateVerificationToken();
        
        db.prepare(`INSERT INTO users (client_id, name, email, phone, status, company_name, country, state, company_code, 
                    password_hash, email_verified, verification_token, can_view_history, can_view_devices, has_subdomain, custom_banner, activation_date) 
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
          .run(client_id, name, email, phone, finalStatus, company_name, country, state, 
               company_code ? company_code.toUpperCase() : null, password_hash, email_verified, verification_token,
               can_view_history, can_view_devices, has_subdomain, custom_banner, new Date().toISOString());
        
        // Si no se puso contraseña, enviar email de verificación
        if (!password && verification_token) {
            sendVerificationEmail(email, name, verification_token).catch(e => {
                log('error', `Error enviando email de verificación: ${e.message}`);
            });
        }
        
        res.redirect('/admin/users');
    } catch (err) {
        res.send(`<div style="padding:40px;color:#ff4757">❌ Error: ${err.message} <a href="/admin/users/new" style="color:#00d4ff">Volver</a></div>`);
    }
});

app.get('/admin/users/:id/edit', authMiddleware, (req, res) => {
    const user = db.prepare('SELECT * FROM users WHERE id = ?').get(req.params.id);
    if (!user) return res.status(404).send('Usuario no encontrado');
    
    res.send(`<!DOCTYPE html><html><head><title>Editar Usuario - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('users')}
        <h2 style="margin-bottom:20px">✏️ Editar Usuario: ${user.name}</h2>
        <div class="card">
            <form method="POST">
                <div class="form-row">
                    <div class="form-group"><label>ID Cliente *</label><input name="client_id" value="${user.client_id}" required></div>
                    <div class="form-group"><label>Nombre *</label><input name="name" value="${user.name}" required></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>Email *</label><input name="email" type="email" value="${user.email}" required></div>
                    <div class="form-group"><label>Teléfono</label><input name="phone" value="${user.phone || ''}"></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>Empresa</label><input name="company_name" value="${user.company_name || ''}" placeholder="Nombre de empresa"></div>
                    <div class="form-group"><label>Nueva Contraseña</label><input name="password" type="password" placeholder="Dejar vacío para no cambiar"></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>País</label><input name="country" value="${user.country || ''}" placeholder="Ej: México"></div>
                    <div class="form-group"><label>Estado/Provincia</label><input name="state" value="${user.state || ''}" placeholder="Ej: Jalisco"></div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>Código de Empresa (4 dígitos)</label>
                        <div style="display:flex;gap:10px">
                            <input name="company_code" id="company_code" value="${user.company_code || ''}" maxlength="4" style="text-transform:uppercase;font-family:Consolas;font-size:18px;letter-spacing:4px" placeholder="XXXX">
                            <button type="button" onclick="genCode()" class="btn btn-success">🎲 Generar</button>
                        </div>
                    </div>
                    <div class="form-group">
                        <label>Contraseña de Servicio (5 dígitos)</label>
                        <div style="display:flex;gap:10px">
                            <input name="service_password" id="service_password" value="${user.service_password || ''}" maxlength="5" style="text-transform:uppercase;font-family:Consolas;font-size:18px;letter-spacing:3px" placeholder="XXXXX">
                            <button type="button" onclick="genServicePwd()" class="btn btn-success">🔑 Generar</button>
                        </div>
                        <small style="color:#888">Requerida para instalar como servicio o usar código fijo</small>
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>Estado de cuenta</label>
                        <select name="status">
                            <option value="active" ${user.status === 'active' ? 'selected' : ''}>✅ Activo</option>
                            <option value="pending" ${user.status === 'pending' ? 'selected' : ''}>⏳ Pendiente</option>
                            <option value="disabled" ${user.status === 'disabled' ? 'selected' : ''}>🚫 Deshabilitado</option>
                        </select>
                    </div>
                    <div class="form-group"></div>
                </div>
                <div style="background:#16213e;padding:15px;border-radius:8px;margin:15px 0">
                    <h4 style="margin-bottom:10px">🔐 Permisos del Usuario</h4>
                    <label style="display:flex;align-items:center;gap:10px;margin-bottom:8px;cursor:pointer">
                        <input type="checkbox" name="can_view_history" value="1" ${user.can_view_history ? 'checked' : ''}> 📜 Puede ver historial de conexiones
                    </label>
                    <label style="display:flex;align-items:center;gap:10px;margin-bottom:8px;cursor:pointer">
                        <input type="checkbox" name="can_view_devices" value="1" ${user.can_view_devices ? 'checked' : ''}> 💻 Puede ver dispositivos
                    </label>
                    <label style="display:flex;align-items:center;gap:10px;margin-bottom:8px;cursor:pointer">
                        <input type="checkbox" name="has_subdomain" value="1" ${user.has_subdomain ? 'checked' : ''}> 🌐 Tiene subdominio personalizado
                    </label>
                    <label style="display:flex;align-items:center;gap:10px;cursor:pointer">
                        <input type="checkbox" name="custom_banner" value="1" ${user.custom_banner ? 'checked' : ''}> 🖼️ Puede personalizar banner
                    </label>
                </div>
                <button type="submit" class="btn btn-primary">💾 Guardar</button>
                <a href="/admin/users" class="btn" style="background:#333;color:#fff;margin-left:10px">Cancelar</a>
            </form>
        </div>
        
        <div class="card" style="margin-top:20px;background:linear-gradient(135deg, #0f3460 0%, #1a1a2e 100%)">
            <h4 style="margin-bottom:15px">📥 Información de Distribución</h4>
            ${user.company_code ? `
            <p style="color:#888;margin-bottom:10px">📁 Nombre del ejecutable: <code style="font-size:16px">Vicviewer${user.company_code}.exe</code></p>
            <p style="color:#888;margin-bottom:10px">🔗 Enlace de descarga: <a href="/download/${user.company_code}" style="color:#00d4ff" target="_blank">/download/${user.company_code}</a></p>
            ${user.has_subdomain ? `<p style="color:#888">🌐 Subdominio: <a href="https://${user.company_code.toLowerCase()}.vicviewer.com" style="color:#00d4ff" target="_blank">${user.company_code.toLowerCase()}.vicviewer.com</a></p>` : ''}
            ` : '<p style="color:#ff6b6b">⚠️ Asigna un código de empresa para generar enlace de descarga</p>'}
        </div>
        
        <div class="card" style="margin-top:20px">
            <h4 style="margin-bottom:15px">📋 Información Adicional</h4>
            <table style="font-size:14px">
                <tr><td style="color:#888;padding-right:20px">Registrado:</td><td>${user.created_at ? new Date(user.created_at).toLocaleString() : '-'}</td></tr>
                <tr><td style="color:#888">Email verificado:</td><td>${user.email_verified ? '✅ Sí' : '❌ No'}</td></tr>
                <tr><td style="color:#888">Última conexión:</td><td>${user.last_connection ? new Date(user.last_connection).toLocaleString() : '-'}</td></tr>
                <tr><td style="color:#888">Último login:</td><td>${user.last_login ? new Date(user.last_login).toLocaleString() : '-'}</td></tr>
            </table>
        </div>
    </div>
    <script>
        function genCode() {
            if (!confirm('⚠️ ADVERTENCIA\n\nGenerar nuevo código de empresa:\n• Cambiará el enlace de descarga\n• Cambiará el subdominio (si aplica)\n• Los usuarios deberán usar el nuevo código\n\n¿Desea continuar?')) return;
            const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ0123456789';
            let code = '';
            for(let i=0;i<4;i++) code += chars[Math.floor(Math.random()*chars.length)];
            document.getElementById('company_code').value = code;
        }
        function genServicePwd() {
            if (!confirm('⚠️ ADVERTENCIA\n\nGenerar nueva contraseña de servicio:\n• La contraseña anterior dejará de funcionar\n• Deberá actualizar equipos con servicio instalado\n\n¿Desea continuar?')) return;
            const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZ0123456789';
            let pwd = '';
            for(let i=0;i<5;i++) pwd += chars[Math.floor(Math.random()*chars.length)];
            document.getElementById('service_password').value = pwd;
        }
    </script>
    </body></html>`);
});

app.post('/admin/users/:id/edit', authMiddleware, (req, res) => {
    const { client_id, name, email, phone, status, company_name, country, state, company_code, password, service_password } = req.body;
    const can_view_history = req.body.can_view_history ? 1 : 0;
    const can_view_devices = req.body.can_view_devices ? 1 : 0;
    const has_subdomain = req.body.has_subdomain ? 1 : 0;
    const custom_banner = req.body.custom_banner ? 1 : 0;
    
    try {
        // Verificar que el código de empresa no exista en otro usuario
        if (company_code) {
            const existing = db.prepare('SELECT id FROM users WHERE company_code = ? AND id != ?').get(company_code.toUpperCase(), req.params.id);
            if (existing) {
                return res.send(`<div style="padding:40px;color:#ff4757">❌ El código de empresa "${company_code}" ya está en uso por otro usuario. <a href="/admin/users/${req.params.id}/edit" style="color:#00d4ff">Volver</a></div>`);
            }
        }
        
        // Si se proporcionó nueva contraseña, actualizarla
        if (password) {
            const password_hash = crypto.createHash('sha256').update(password).digest('hex');
            db.prepare('UPDATE users SET password_hash=?, email_verified=1 WHERE id=?').run(password_hash, req.params.id);
        }
        
        // Actualizar service_password si se proporcionó
        if (service_password) {
            db.prepare('UPDATE users SET service_password=? WHERE id=?').run(service_password.toUpperCase(), req.params.id);
        }
        
        db.prepare(`UPDATE users SET client_id=?, name=?, email=?, phone=?, status=?, company_name=?, country=?, state=?, 
                    company_code=?, can_view_history=?, can_view_devices=?, has_subdomain=?, custom_banner=?, updated_at=? WHERE id=?`)
          .run(client_id, name, email, phone, status, company_name, country, state, 
               company_code ? company_code.toUpperCase() : null, can_view_history, can_view_devices, has_subdomain, custom_banner,
               new Date().toISOString(), req.params.id);
        res.redirect('/admin/users');
    } catch (err) {
        res.send(`<div style="padding:40px;color:#ff4757">❌ Error: ${err.message} <a href="/admin/users/${req.params.id}/edit" style="color:#00d4ff">Volver</a></div>`);
    }
});

// ============== CONFIGURACIÓN ==============
app.get('/admin/config', authMiddleware, (req, res) => {
    const success = req.query.success;
    
    res.send(`<!DOCTYPE html><html><head><title>Configuración - Vicviewer®</title>${adminCSS}</head><body>
    <div class="container">${adminHeader('config')}
        <h2 style="margin-bottom:20px">⚙️ Configuración</h2>
        
        ${success ? '<div class="alert alert-success">✅ Configuración guardada correctamente</div>' : ''}
        
        <div class="tabs">
            <a href="#admin" class="active" onclick="showTab('admin')">🔐 Admin</a>
            <a href="#smtp" onclick="showTab('smtp')">📧 SMTP</a>
        </div>
        
        <div id="tab-admin" class="card">
            <h3 style="margin-bottom:15px">Credenciales de Administrador</h3>
            <form method="POST" action="/admin/config/admin">
                <div class="form-row">
                    <div class="form-group">
                        <label>Usuario</label>
                        <input name="admin_user" value="${getConfig('admin_user')}" required>
                    </div>
                    <div class="form-group">
                        <label>Nueva Contraseña (dejar vacío para no cambiar)</label>
                        <input name="admin_pass" type="password" placeholder="••••••••">
                    </div>
                </div>
                <button type="submit" class="btn btn-primary">💾 Guardar Credenciales</button>
            </form>
        </div>
        
        <div id="tab-smtp" class="card" style="display:none">
            <h3 style="margin-bottom:15px">Configuración de Email (SMTP)</h3>
            <form method="POST" action="/admin/config/smtp">
                <div class="form-row">
                    <div class="form-group">
                        <label>Servidor SMTP</label>
                        <input name="smtp_host" value="${getConfig('smtp_host')}" required placeholder="mail.ejemplo.com">
                    </div>
                    <div class="form-group">
                        <label>Puerto</label>
                        <input name="smtp_port" value="${getConfig('smtp_port')}" required placeholder="465">
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>Usuario SMTP</label>
                        <input name="smtp_user" value="${getConfig('smtp_user')}" required placeholder="usuario@ejemplo.com">
                    </div>
                    <div class="form-group">
                        <label>Contraseña SMTP</label>
                        <input name="smtp_pass" type="password" value="${getConfig('smtp_pass')}" required>
                    </div>
                </div>
                <div class="form-group">
                    <label>Conexión Segura (SSL/TLS)</label>
                    <select name="smtp_secure">
                        <option value="true" ${getConfig('smtp_secure') === 'true' ? 'selected' : ''}>Sí (Puerto 465)</option>
                        <option value="false" ${getConfig('smtp_secure') !== 'true' ? 'selected' : ''}>No (Puerto 587)</option>
                    </select>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>Nombre del Remitente</label>
                        <input name="smtp_from_name" value="${getConfig('smtp_from_name')}" required placeholder="Vicviewer®">
                    </div>
                    <div class="form-group">
                        <label>Email del Remitente</label>
                        <input name="smtp_from_email" value="${getConfig('smtp_from_email')}" required placeholder="noreply@ejemplo.com">
                    </div>
                </div>
                <button type="submit" class="btn btn-primary">💾 Guardar SMTP</button>
                <button type="button" onclick="testEmail()" class="btn btn-success" style="margin-left:10px">📧 Probar Email</button>
            </form>
        </div>
    </div>
    <script>
        function showTab(tab) {
            document.querySelectorAll('.tabs a').forEach(a => a.classList.remove('active'));
            document.querySelector('.tabs a[href="#'+tab+'"]').classList.add('active');
            document.getElementById('tab-admin').style.display = tab === 'admin' ? 'block' : 'none';
            document.getElementById('tab-smtp').style.display = tab === 'smtp' ? 'block' : 'none';
        }
        function testEmail() {
            const email = prompt('Enviar email de prueba a:');
            if (email) {
                fetch('/admin/config/test-email', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({email})
                }).then(r => r.json()).then(data => {
                    alert(data.success ? '✅ Email enviado correctamente' : '❌ Error: ' + data.error);
                });
            }
        }
    </script>
    </body></html>`);
});

app.post('/admin/config/admin', authMiddleware, (req, res) => {
    const { admin_user, admin_pass } = req.body;
    setConfig('admin_user', admin_user);
    if (admin_pass && admin_pass.length > 0) {
        const hash = crypto.createHash('sha256').update(admin_pass).digest('hex');
        setConfig('admin_pass_hash', hash);
    }
    res.redirect('/admin/config?success=1');
});

app.post('/admin/config/smtp', authMiddleware, (req, res) => {
    const { smtp_host, smtp_port, smtp_user, smtp_pass, smtp_secure, smtp_from_name, smtp_from_email } = req.body;
    setConfig('smtp_host', smtp_host);
    setConfig('smtp_port', smtp_port);
    setConfig('smtp_user', smtp_user);
    setConfig('smtp_pass', smtp_pass);
    setConfig('smtp_secure', smtp_secure);
    setConfig('smtp_from_name', smtp_from_name);
    setConfig('smtp_from_email', smtp_from_email);
    res.redirect('/admin/config?success=1');
});

app.post('/admin/config/test-email', authMiddleware, async (req, res) => {
    const { email } = req.body;
    try {
        const transporter = createTransporter();
        await transporter.sendMail({
            from: `"${getConfig('smtp_from_name')}" <${getConfig('smtp_from_email')}>`,
            to: email,
            subject: '✅ Prueba de Email - Vicviewer®',
            html: '<h1>¡Funciona!</h1><p>La configuración SMTP es correcta.</p>'
        });
        res.json({ success: true });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

// ============== API DISPOSITIVOS ==============
// Lista dispositivos de un usuario
app.get('/api/devices', (req, res) => {
    const { clientId } = req.query;
    let devices;
    if (clientId) {
        devices = db.prepare('SELECT * FROM devices WHERE client_id = ? ORDER BY created_at DESC').all(clientId);
    } else {
        devices = db.prepare('SELECT * FROM devices ORDER BY created_at DESC').all();
    }
    res.json(devices);
});

// Endpoint para heartbeat (el host envía señal de vida)
app.post('/heartbeat', (req, res) => {
    const { code, clientId } = req.body;
    
    if (!code) {
        return res.status(400).json({ error: 'code required' });
    }
    
    // Actualizar sesión en memoria
    const session = sessions.get(code);
    if (session) {
        session.lastHeartbeat = Date.now();
    }
    
    // Actualizar dispositivo en BD si es código fijo
    const result = db.prepare(`UPDATE devices SET 
        is_online = 1, 
        last_seen = ? 
        WHERE device_code = ?`).run(new Date().toISOString(), code);
    
    log('debug', `💓 Heartbeat: ${code}`);
    res.json({ success: true });
});

// Endpoint para desconexión limpia
app.post('/disconnect', (req, res) => {
    const { code, clientId } = req.body;
    
    if (!code) {
        return res.status(400).json({ error: 'code required' });
    }
    
    log('info', `🔌 Desconexión limpia: ${code}`);
    
    // Eliminar sesión de memoria
    sessions.delete(code);
    
    // Marcar dispositivo como offline si es fijo
    db.prepare('UPDATE devices SET is_online = 0, last_seen = ? WHERE device_code = ?')
      .run(new Date().toISOString(), code);
    
    // Registrar en historial
    if (clientId) {
        db.prepare('INSERT INTO connection_history (client_id, access_code, action) VALUES (?, ?, ?)')
          .run(clientId, code, 'host_disconnected');
    }
    
    res.json({ success: true });
});

// ============== PRE-REGISTRO DE DISPOSITIVO (desde UI antes de instalar servicio) ==============
app.post('/api/devices/register', express.json(), (req, res) => {
    const { code, clientId, deviceName } = req.body;  // clientId es en realidad companyCode
    const ipAddress = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
    
    if (!code || !clientId) {
        return res.status(400).json({ error: 'Se requiere code y clientId' });
    }
    
    log('info', `📱 Pre-registro dispositivo: ${code} para empresa ${clientId}`);
    
    // Buscar usuario por company_code para obtener el client_id real (FK)
    const user = db.prepare('SELECT id, client_id FROM users WHERE company_code = ?').get(clientId);
    if (!user) {
        log('error', `❌ No se encontró usuario con company_code: ${clientId}`);
        return res.status(404).json({ error: 'client_not_found', message: 'No se encontró el cliente' });
    }
    
    const realClientId = user.client_id;  // Este es el ID real que referencia la FK
    log('info', `🔍 Encontrado usuario - company_code: ${clientId} -> client_id: ${realClientId}`);
    
    // Verificar si el código ya existe
    const existing = db.prepare('SELECT * FROM devices WHERE device_code = ?').get(code);
    
    if (existing) {
        if (existing.client_id != realClientId) {
            return res.status(409).json({ error: 'code_in_use', message: 'Este código ya está en uso por otro usuario' });
        }
        // Actualizar dispositivo existente
        db.prepare(`UPDATE devices SET 
            device_name = COALESCE(?, device_name),
            ip_address = ?,
            is_online = 0,
            updated_at = ?
            WHERE device_code = ?`).run(
            deviceName,
            ipAddress,
            new Date().toISOString(),
            code
        );
        log('info', `🔄 Dispositivo actualizado (pre-registro): ${code}`);
    } else {
        // Crear nuevo dispositivo con el client_id real
        db.prepare(`INSERT INTO devices (client_id, device_code, device_name, ip_address, is_online, is_service, last_seen)
            VALUES (?, ?, ?, ?, 0, 1, ?)`).run(
            realClientId,
            code,
            deviceName || `Equipo ${code}`,
            ipAddress,
            new Date().toISOString()
        );
        log('info', `✨ Nuevo dispositivo pre-registrado: ${code}`);
    }
    
    res.json({ success: true, code, message: 'Dispositivo pre-registrado' });
});

// Eliminar dispositivo
app.delete('/api/devices/:code', (req, res) => {
    const code = req.params.code;
    sessions.delete(code);
    db.prepare('DELETE FROM devices WHERE device_code = ?').run(code);
    log('info', `🗑️ Dispositivo eliminado: ${code}`);
    res.json({ success: true });
});

// Limpieza de sesiones expiradas
setInterval(() => {
    const now = Date.now();
    let removed = 0;
    for (const [code, session] of sessions) {
        // No eliminar sesiones con código fijo
        if (!session.isFixedCode && now > session.expiresAt) {
            sessions.delete(code);
            removed++;
        }
    }
    if (removed > 0) log('info', `🧹 ${removed} sesiones expiradas eliminadas`);
}, 30000);

// Marcar dispositivos offline si no hay heartbeat en 5 minutos
setInterval(() => {
    const fiveMinutesAgo = new Date(Date.now() - 90 * 1000).toISOString();
    const result = db.prepare('UPDATE devices SET is_online = 0 WHERE is_online = 1 AND last_seen < ?').run(fiveMinutesAgo);
    if (result.changes > 0) {
        log('info', `🔌 ${result.changes} dispositivo(s) marcado(s) como offline`);
    }
}, 30000);

// Limpieza periódica del historial (cada hora, mantener solo últimos 100 por usuario)
setInterval(() => {
    cleanupHistory();
}, 60 * 60 * 1000);

// ============== ESTILOS CSS PANEL DE USUARIO ==============
const userCSS = `
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
<style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: 'Segoe UI', sans-serif; background: #0f0f1a; color: #eee; min-height: 100vh; }
    .container { max-width: 1000px; margin: 0 auto; padding: 20px; }
    .header { display: flex; justify-content: space-between; align-items: center; padding: 20px 0; border-bottom: 1px solid #333; margin-bottom: 30px; }
    .logo { font-size: 24px; font-weight: bold; color: #00d4ff; }
    .nav a { color: #888; text-decoration: none; margin-left: 20px; } .nav a:hover { color: #fff; }
    .nav .logout { color: #ff6b6b; }
    .card { background: #1a1a2e; border-radius: 12px; padding: 25px; margin-bottom: 20px; }
    .card h3 { color: #00d4ff; margin-bottom: 15px; }
    .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 15px; margin-bottom: 25px; }
    .stat { background: linear-gradient(135deg, #16213e 0%, #1a1a2e 100%); border-radius: 10px; padding: 20px; text-align: center; }
    .stat-value { font-size: 32px; font-weight: bold; color: #00d4ff; }
    .stat-label { color: #888; font-size: 13px; margin-top: 5px; }
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 10px 12px; text-align: left; border-bottom: 1px solid #333; font-size: 14px; }
    th { color: #888; font-weight: 500; }
    tr:hover { background: #16213e; }
    .btn { padding: 8px 16px; border: none; border-radius: 6px; cursor: pointer; font-size: 14px; text-decoration: none; display: inline-block; }
    .btn-primary { background: #00d4ff; color: #000; } .btn-primary:hover { background: #00b8e6; }
    .btn-danger { background: #ff4757; color: #fff; }
    .btn-sm { padding: 4px 10px; font-size: 12px; }
    .status { padding: 4px 10px; border-radius: 20px; font-size: 12px; display: inline-block; }
    .status-online { background: #10b98122; color: #10b981; }
    .status-offline { background: #66666622; color: #888; }
    .form-group { margin-bottom: 15px; }
    .form-group label { display: block; margin-bottom: 5px; color: #888; }
    .form-group input, .form-group select { width: 100%; padding: 10px; border: 1px solid #333; border-radius: 6px; background: #16213e; color: #eee; }
    .form-row { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; }
    code { background: #16213e; padding: 2px 8px; border-radius: 4px; font-family: Consolas; }
    .alert { padding: 15px; border-radius: 8px; margin-bottom: 20px; }
    .alert-success { background: #10b98122; border: 1px solid #10b981; color: #10b981; }
    .alert-error { background: #ff475722; border: 1px solid #ff4757; color: #ff4757; }
    .alert-info { background: #00d4ff22; border: 1px solid #00d4ff; color: #00d4ff; }
    .login-container { display: flex; justify-content: center; align-items: center; min-height: 100vh; }
    .login-box { background: #1a1a2e; padding: 40px; border-radius: 16px; width: 100%; max-width: 400px; }
    .login-box h1 { text-align: center; margin-bottom: 30px; color: #00d4ff; }
    .login-box .btn { width: 100%; padding: 12px; }
    .login-links { text-align: center; margin-top: 20px; }
    .login-links a { color: #00d4ff; text-decoration: none; margin: 0 10px; font-size: 14px; }
    .empty { text-align: center; padding: 40px; color: #666; }
    .download-box { background: linear-gradient(135deg, #0f3460 0%, #1a1a2e 100%); border: 2px solid #00d4ff; border-radius: 12px; padding: 30px; text-align: center; }
    .download-box h2 { color: #00d4ff; margin-bottom: 15px; }
    .download-box .code { font-size: 48px; font-weight: bold; color: #fff; letter-spacing: 8px; margin: 20px 0; font-family: Consolas; }
</style>`;

const userHeader = (user, currentPath = '') => `
<div class="header">
    <div class="logo">🖥️ Vicviewer®</div>
    <nav class="nav">
        <a href="/panel" ${currentPath === 'dashboard' ? 'style="color:#00d4ff"' : ''}>Inicio</a>
        ${user.can_view_devices ? `<a href="/panel/devices" ${currentPath === 'devices' ? 'style="color:#00d4ff"' : ''}>Dispositivos</a>` : ''}
        ${user.can_view_history ? `<a href="/panel/history" ${currentPath === 'history' ? 'style="color:#00d4ff"' : ''}>Historial</a>` : ''}
        <a href="/panel/profile" ${currentPath === 'profile' ? 'style="color:#00d4ff"' : ''}>Mi Perfil</a>
        <a href="/logout" class="logout">🚪 Salir</a>
    </nav>
</div>`;

// ============== RUTAS DE AUTENTICACIÓN DE USUARIO ==============
app.get('/login', (req, res) => {
    const error = req.query.error;
    const success = req.query.success;
    res.send(`<!DOCTYPE html><html><head><title>Iniciar Sesión - Vicviewer®</title>${userCSS}</head><body>
    <div class="login-container">
        <div class="login-box">
            <h1>🖥️ Vicviewer®</h1>
            ${error ? '<div class="alert alert-error">Credenciales incorrectas</div>' : ''}
            ${success === 'verified' ? '<div class="alert alert-success">✅ Email verificado. Ya puedes iniciar sesión.</div>' : ''}
            ${success === 'reset' ? '<div class="alert alert-success">✅ Contraseña actualizada. Ya puedes iniciar sesión.</div>' : ''}
            <form method="POST" action="/login">
                <div class="form-group">
                    <label>Email</label>
                    <input name="email" type="email" required autofocus placeholder="tu@email.com">
                </div>
                <div class="form-group">
                    <label>Contraseña</label>
                    <input name="password" type="password" required placeholder="••••••••">
                </div>
                <button type="submit" class="btn btn-primary">Iniciar Sesión</button>
            </form>
            <div class="login-links">
                <a href="/register">Crear cuenta</a>
                <a href="/forgot-password">¿Olvidaste tu contraseña?</a>
            </div>
        </div>
    </div>
    </body></html>`);
});

app.post('/login', (req, res) => {
    const { email, password } = req.body;
    const user = db.prepare('SELECT * FROM users WHERE email = ?').get(email);
    
    if (!user || !user.password_hash) {
        return res.redirect('/login?error=1');
    }
    
    const hash = crypto.createHash('sha256').update(password).digest('hex');
    if (hash !== user.password_hash) {
        return res.redirect('/login?error=1');
    }
    
    if (!user.email_verified) {
        return res.send(`<!DOCTYPE html><html><head><title>Verificación Pendiente</title>${userCSS}</head><body>
        <div class="login-container">
            <div class="login-box">
                <h1>📧 Verificación Pendiente</h1>
                <div class="alert alert-info">Tu correo aún no ha sido verificado. Revisa tu bandeja de entrada.</div>
                <a href="/login" class="btn btn-primary" style="margin-top:20px">Volver</a>
            </div>
        </div></body></html>`);
    }
    
    if (user.status !== 'active') {
        return res.send(`<!DOCTYPE html><html><head><title>Cuenta Inactiva</title>${userCSS}</head><body>
        <div class="login-container">
            <div class="login-box">
                <h1>⚠️ Cuenta Inactiva</h1>
                <div class="alert alert-error">Tu cuenta no está activa. Contacta al administrador.</div>
                <a href="/login" class="btn btn-primary" style="margin-top:20px">Volver</a>
            </div>
        </div></body></html>`);
    }
    
    const token = createUserSession(user.id);
    db.prepare('UPDATE users SET last_login = ? WHERE id = ?').run(new Date().toISOString(), user.id);
    
    res.cookie('user_token', token, { httpOnly: true, maxAge: USER_SESSION_TTL, sameSite: 'strict' });
    res.redirect('/panel');
});

app.get('/register', (req, res) => {
    const error = req.query.error;
    res.send(`<!DOCTYPE html><html><head><title>Registro - Vicviewer®</title>${userCSS}</head><body>
    <div class="login-container">
        <div class="login-box">
            <h1>🖥️ Crear Cuenta</h1>
            ${error === 'exists' ? '<div class="alert alert-error">Este email ya está registrado</div>' : ''}
            ${error === 'password' ? '<div class="alert alert-error">Las contraseñas no coinciden</div>' : ''}
            <form method="POST" action="/register">
                <div class="form-group">
                    <label>Nombre completo *</label>
                    <input name="name" type="text" required placeholder="Tu nombre">
                </div>
                <div class="form-group">
                    <label>Email *</label>
                    <input name="email" type="email" required placeholder="tu@email.com">
                </div>
                <div class="form-group">
                    <label>Contraseña *</label>
                    <input name="password" type="password" required minlength="6" placeholder="Mínimo 6 caracteres">
                </div>
                <div class="form-group">
                    <label>Confirmar contraseña *</label>
                    <input name="password2" type="password" required placeholder="Repetir contraseña">
                </div>
                <button type="submit" class="btn btn-primary">Registrarme</button>
            </form>
            <div class="login-links">
                <a href="/login">Ya tengo cuenta</a>
            </div>
        </div>
    </div>
    </body></html>`);
});

app.post('/register', async (req, res) => {
    const { name, email, password, password2 } = req.body;
    
    if (password !== password2) {
        return res.redirect('/register?error=password');
    }
    
    const existing = db.prepare('SELECT 1 FROM users WHERE email = ?').get(email);
    if (existing) {
        return res.redirect('/register?error=exists');
    }
    
    const hash = crypto.createHash('sha256').update(password).digest('hex');
    const token = generateVerificationToken();
    const clientId = 'pending_' + Date.now();
    
    try {
        db.prepare(`INSERT INTO users (client_id, name, email, password_hash, verification_token, status) VALUES (?, ?, ?, ?, ?, 'pending')`)
          .run(clientId, name, email, hash, token);
        
        await sendVerificationEmail(email, name, token);
        
        res.send(`<!DOCTYPE html><html><head><title>Verifica tu email</title>${userCSS}</head><body>
        <div class="login-container">
            <div class="login-box">
                <h1>📧 Revisa tu correo</h1>
                <div class="alert alert-success">Te enviamos un enlace de verificación a <strong>${email}</strong></div>
                <p style="color:#888;margin-top:20px;text-align:center">Revisa también la carpeta de spam.</p>
                <a href="/login" class="btn btn-primary" style="margin-top:20px">Ir a Login</a>
            </div>
        </div></body></html>`);
    } catch (err) {
        res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container">
            <div class="login-box">
                <h1>❌ Error</h1>
                <div class="alert alert-error">${err.message}</div>
                <a href="/register" class="btn btn-primary">Volver</a>
            </div>
        </div></body></html>`);
    }
});

app.get('/verify-email', (req, res) => {
    const { token } = req.query;
    if (!token) return res.redirect('/login');
    
    const user = db.prepare('SELECT * FROM users WHERE verification_token = ?').get(token);
    if (!user) {
        return res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container">
            <div class="login-box">
                <h1>❌ Enlace inválido</h1>
                <div class="alert alert-error">El enlace de verificación no es válido o ya fue usado.</div>
                <a href="/login" class="btn btn-primary">Ir a Login</a>
            </div>
        </div></body></html>`);
    }
    
    db.prepare('UPDATE users SET email_verified = 1, verification_token = NULL, status = ? WHERE id = ?')
      .run('active', user.id);
    
    res.redirect('/login?success=verified');
});

app.get('/forgot-password', (req, res) => {
    const sent = req.query.sent;
    res.send(`<!DOCTYPE html><html><head><title>Recuperar Contraseña</title>${userCSS}</head><body>
    <div class="login-container">
        <div class="login-box">
            <h1>🔑 Recuperar Contraseña</h1>
            ${sent ? '<div class="alert alert-success">Si el email existe, te enviamos instrucciones.</div>' : ''}
            <form method="POST" action="/forgot-password">
                <div class="form-group">
                    <label>Email</label>
                    <input name="email" type="email" required placeholder="tu@email.com">
                </div>
                <button type="submit" class="btn btn-primary">Enviar instrucciones</button>
            </form>
            <div class="login-links">
                <a href="/login">Volver a Login</a>
            </div>
        </div>
    </div>
    </body></html>`);
});

app.post('/forgot-password', async (req, res) => {
    const { email } = req.body;
    const user = db.prepare('SELECT * FROM users WHERE email = ?').get(email);
    
    if (user) {
        const token = generateVerificationToken();
        const expires = new Date(Date.now() + 60 * 60 * 1000).toISOString(); // 1 hora
        db.prepare('UPDATE users SET reset_token = ?, reset_token_expires = ? WHERE id = ?')
          .run(token, expires, user.id);
        
        try {
            await sendPasswordResetEmail(email, user.name, token);
        } catch(e) {
            log('error', `Error enviando email de reset: ${e.message}`);
        }
    }
    
    res.redirect('/forgot-password?sent=1');
});

app.get('/reset-password', (req, res) => {
    const { token } = req.query;
    const error = req.query.error;
    
    if (!token) return res.redirect('/login');
    
    const user = db.prepare('SELECT * FROM users WHERE reset_token = ? AND reset_token_expires > ?')
      .get(token, new Date().toISOString());
    
    if (!user) {
        return res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container">
            <div class="login-box">
                <h1>❌ Enlace expirado</h1>
                <div class="alert alert-error">Este enlace ha expirado o ya fue usado.</div>
                <a href="/forgot-password" class="btn btn-primary">Solicitar nuevo enlace</a>
            </div>
        </div></body></html>`);
    }
    
    res.send(`<!DOCTYPE html><html><head><title>Nueva Contraseña</title>${userCSS}</head><body>
    <div class="login-container">
        <div class="login-box">
            <h1>🔑 Nueva Contraseña</h1>
            ${error ? '<div class="alert alert-error">Las contraseñas no coinciden</div>' : ''}
            <form method="POST" action="/reset-password">
                <input type="hidden" name="token" value="${token}">
                <div class="form-group">
                    <label>Nueva contraseña</label>
                    <input name="password" type="password" required minlength="6" placeholder="Mínimo 6 caracteres">
                </div>
                <div class="form-group">
                    <label>Confirmar contraseña</label>
                    <input name="password2" type="password" required placeholder="Repetir contraseña">
                </div>
                <button type="submit" class="btn btn-primary">Cambiar contraseña</button>
            </form>
        </div>
    </div>
    </body></html>`);
});

app.post('/reset-password', (req, res) => {
    const { token, password, password2 } = req.body;
    
    if (password !== password2) {
        return res.redirect(`/reset-password?token=${token}&error=1`);
    }
    
    const user = db.prepare('SELECT * FROM users WHERE reset_token = ? AND reset_token_expires > ?')
      .get(token, new Date().toISOString());
    
    if (!user) return res.redirect('/login');
    
    const hash = crypto.createHash('sha256').update(password).digest('hex');
    db.prepare('UPDATE users SET password_hash = ?, reset_token = NULL, reset_token_expires = NULL WHERE id = ?')
      .run(hash, user.id);
    
    res.redirect('/login?success=reset');
});

app.get('/logout', (req, res) => {
    const token = req.cookies?.user_token;
    if (token) {
        db.prepare('DELETE FROM user_sessions WHERE token = ?').run(token);
    }
    res.clearCookie('user_token');
    res.redirect('/login');
});

// ============== PANEL DE USUARIO ==============
app.get('/panel', userAuthMiddleware, (req, res) => {
    const user = req.user;
    const devicesCount = db.prepare('SELECT COUNT(*) as c FROM devices WHERE client_id = ?').get(user.client_id)?.c || 0;
    const devicesOnline = db.prepare('SELECT COUNT(*) as c FROM devices WHERE client_id = ? AND is_online = 1').get(user.client_id)?.c || 0;
    
    res.send(`<!DOCTYPE html><html><head><title>Panel - Vicviewer®</title>${userCSS}</head><body>
    <div class="container">${userHeader(user, 'dashboard')}
        <h2 style="margin-bottom:20px">👋 Bienvenido, ${user.name}</h2>
        
        ${user.company_code ? `
        <div class="download-box" style="margin-bottom:25px">
            <h2>📥 Tu Código de Empresa</h2>
            <div class="code">${user.company_code}</div>
            <p style="color:#888;margin-bottom:15px">Este código identifica tu empresa en Vicviewer®</p>
            ${user.has_subdomain ? `
            <p style="margin-top:15px"><strong>Tu enlace personalizado:</strong><br>
            <a href="https://${user.company_code.toLowerCase()}.vicviewer.com" style="color:#00d4ff">${user.company_code.toLowerCase()}.vicviewer.com</a></p>
            ` : ''}
        </div>
        ` : `
        <div class="alert alert-info">Tu código de empresa aún no ha sido asignado. Contacta al administrador.</div>
        `}
        
        ${user.can_view_devices ? `
        <div class="stats">
            <div class="stat" style="background:linear-gradient(135deg, #064e3b 0%, #1a1a2e 100%)">
                <div class="stat-value">${devicesOnline}</div>
                <div class="stat-label">🟢 Dispositivos Online</div>
            </div>
            <div class="stat">
                <div class="stat-value">${devicesCount}</div>
                <div class="stat-label">📱 Total Dispositivos</div>
            </div>
        </div>
        ` : ''}
        
        <div class="card">
            <h3>📋 Información de tu cuenta</h3>
            <table>
                <tr><td style="color:#888">Email</td><td>${user.email}</td></tr>
                <tr><td style="color:#888">Empresa</td><td>${user.company_name || '-'}</td></tr>
                <tr><td style="color:#888">País</td><td>${user.country || '-'}</td></tr>
                <tr><td style="color:#888">Estado</td><td>${user.state || '-'}</td></tr>
                <tr><td style="color:#888">Miembro desde</td><td>${user.created_at ? new Date(user.created_at).toLocaleDateString() : '-'}</td></tr>
            </table>
        </div>
    </div>
    </body></html>`);
});

app.get('/panel/profile', userAuthMiddleware, (req, res) => {
    const user = req.user;
    const success = req.query.success;
    
    res.send(`<!DOCTYPE html><html><head><title>Mi Perfil - Vicviewer®</title>${userCSS}</head><body>
    <div class="container">${userHeader(user, 'profile')}
        <h2 style="margin-bottom:20px">👤 Mi Perfil</h2>
        ${success ? '<div class="alert alert-success">✅ Cambios guardados correctamente</div>' : ''}
        
        <div class="card">
            <h3>Información Personal</h3>
            <form method="POST" action="/panel/profile">
                <div class="form-row">
                    <div class="form-group"><label>Nombre *</label><input name="name" value="${user.name}" required></div>
                    <div class="form-group"><label>Empresa</label><input name="company_name" value="${user.company_name || ''}" placeholder="Nombre de tu empresa"></div>
                </div>
                <div class="form-row">
                    <div class="form-group"><label>País</label><input name="country" value="${user.country || ''}" placeholder="Ej: México"></div>
                    <div class="form-group"><label>Estado/Provincia</label><input name="state" value="${user.state || ''}" placeholder="Ej: Jalisco"></div>
                </div>
                <div class="form-group"><label>Teléfono</label><input name="phone" value="${user.phone || ''}" placeholder="+52 555 123 4567"></div>
                <button type="submit" class="btn btn-primary">💾 Guardar cambios</button>
            </form>
        </div>
        
        <div class="card">
            <h3>🔑 Cambiar Contraseña</h3>
            <form method="POST" action="/panel/password">
                <div class="form-group"><label>Contraseña actual</label><input name="current" type="password" required></div>
                <div class="form-row">
                    <div class="form-group"><label>Nueva contraseña</label><input name="password" type="password" required minlength="6"></div>
                    <div class="form-group"><label>Confirmar nueva</label><input name="password2" type="password" required></div>
                </div>
                <button type="submit" class="btn btn-primary">🔑 Cambiar contraseña</button>
            </form>
        </div>
        
        ${user.company_code ? \`
        <div class="card">
            <h3>🔐 Contraseña de Servicio</h3>
            <p style="color:#888;margin-bottom:15px">Esta contraseña se requiere para instalar VicViewer como servicio de Windows o usar código fijo en los equipos.</p>
            <div style="display:flex;align-items:center;gap:15px;margin-bottom:15px">
                <div style="background:#16213e;padding:15px 25px;border-radius:8px;border:2px solid #0f3460">
                    <span style="color:#888;font-size:12px;display:block;margin-bottom:5px">Contraseña actual:</span>
                    <code id="servicePwd" style="font-size:24px;letter-spacing:4px;color:#00d4ff">\${user.service_password || '-----'}</code>
                </div>
                <button type="button" onclick="generateServicePwd()" class="btn btn-success" style="padding:15px 20px">🔄 Generar Nueva</button>
            </div>
            <p style="color:#ff6b6b;font-size:13px">⚠️ Si genera una nueva contraseña, la anterior dejará de funcionar inmediatamente.</p>
        </div>
        \` : ''}
        
        ${user.custom_banner && user.company_code ? `
        <div class="card">
            <h3>🖼️ Banner Personalizado</h3>
            <p style="color:#888;margin-bottom:15px">Sube una imagen para mostrar como banner en tu aplicación Vicviewer®. Tamaño recomendado: 460x60 píxeles.</p>
            <div style="margin-bottom:15px">
                <img src="/banners/${user.company_code}.png" alt="Banner actual" style="max-width:460px;height:60px;border:1px solid #333;border-radius:4px;background:#111" onerror="this.style.display='none';document.getElementById('no-banner').style.display='block'">
                <div id="no-banner" style="display:none;padding:15px;background:#16213e;border-radius:4px;color:#888">
                    No tienes un banner personalizado. Se usa el banner por defecto.
                </div>
            </div>
            <form method="POST" action="/panel/banner" enctype="multipart/form-data">
                <div class="form-group">
                    <label>Subir nuevo banner (PNG o JPEG, máx 2MB)</label>
                    <input type="file" name="banner" accept="image/png,image/jpeg" required style="padding:10px;background:#16213e;border:1px solid #333;border-radius:4px;width:100%">
                </div>
                <button type="submit" class="btn btn-primary">📤 Subir Banner</button>
            </form>
        </div>
        ` : ''}
    </div>
    <script>
        async function generateServicePwd() {
            if (!confirm('⚠️ ADVERTENCIA\\n\\nGenerar nueva contraseña de servicio:\\n• La contraseña anterior dejará de funcionar\\n• Deberá actualizar equipos con servicio instalado\\n\\n¿Desea continuar?')) return;
            
            try {
                const res = await fetch('/api/user/generate-service-password', { method: 'POST' });
                const data = await res.json();
                if (data.success) {
                    document.getElementById('servicePwd').textContent = data.servicePassword;
                    alert('✅ Nueva contraseña generada: ' + data.servicePassword);
                } else {
                    alert('❌ Error: ' + (data.error || 'No se pudo generar'));
                }
            } catch(e) {
                alert('❌ Error de conexión');
            }
        }
    </script>
    </body></html>`);
});

app.post('/panel/profile', userAuthMiddleware, (req, res) => {
    const { name, company_name, country, state, phone } = req.body;
    db.prepare('UPDATE users SET name=?, company_name=?, country=?, state=?, phone=?, updated_at=? WHERE id=?')
      .run(name, company_name, country, state, phone, new Date().toISOString(), req.user.id);
    res.redirect('/panel/profile?success=1');
});

// Subir banner personalizado
app.post('/panel/banner', userAuthMiddleware, uploadBanner.single('banner'), (req, res) => {
    const user = req.user;
    
    // Verificar permisos
    if (!user.custom_banner || !user.company_code) {
        return res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container"><div class="login-box">
            <h1>❌ Error</h1>
            <div class="alert alert-error">No tienes permiso para subir banners</div>
            <a href="/panel/profile" class="btn btn-primary">Volver</a>
        </div></div></body></html>`);
    }
    
    if (!req.file) {
        return res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container"><div class="login-box">
            <h1>❌ Error</h1>
            <div class="alert alert-error">No se recibió ningún archivo</div>
            <a href="/panel/profile" class="btn btn-primary">Volver</a>
        </div></div></body></html>`);
    }
    
    try {
        // Renombrar el archivo temporal al nombre final: {company_code}.png
        const finalPath = path.join(BANNERS_DIR, `${user.company_code}.png`);
        
        // Eliminar banner anterior si existe
        if (fs.existsSync(finalPath)) {
            fs.unlinkSync(finalPath);
        }
        
        // Mover el archivo subido
        fs.renameSync(req.file.path, finalPath);
        
        log('info', `🖼️ Banner subido: ${user.company_code}.png por ${user.name}`);
        res.redirect('/panel/profile?success=1');
    } catch (err) {
        log('error', `Error subiendo banner: ${err.message}`);
        // Limpiar archivo temporal si existe
        if (req.file && fs.existsSync(req.file.path)) {
            fs.unlinkSync(req.file.path);
        }
        res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container"><div class="login-box">
            <h1>❌ Error</h1>
            <div class="alert alert-error">Error al subir el banner: ${err.message}</div>
            <a href="/panel/profile" class="btn btn-primary">Volver</a>
        </div></div></body></html>`);
    }
});

app.post('/panel/password', userAuthMiddleware, (req, res) => {
    const { current, password, password2 } = req.body;
    const user = req.user;
    
    const currentHash = crypto.createHash('sha256').update(current).digest('hex');
    if (currentHash !== user.password_hash) {
        return res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container"><div class="login-box">
            <h1>❌ Error</h1>
            <div class="alert alert-error">La contraseña actual es incorrecta</div>
            <a href="/panel/profile" class="btn btn-primary">Volver</a>
        </div></div></body></html>`);
    }
    
    if (password !== password2) {
        return res.send(`<!DOCTYPE html><html><head><title>Error</title>${userCSS}</head><body>
        <div class="login-container"><div class="login-box">
            <h1>❌ Error</h1>
            <div class="alert alert-error">Las contraseñas no coinciden</div>
            <a href="/panel/profile" class="btn btn-primary">Volver</a>
        </div></div></body></html>`);
    }
    
    const newHash = crypto.createHash('sha256').update(password).digest('hex');
    db.prepare('UPDATE users SET password_hash = ? WHERE id = ?').run(newHash, user.id);
    res.redirect('/panel/profile?success=1');
});

app.get('/panel/devices', userAuthMiddleware, (req, res) => {
    const user = req.user;
    
    if (!user.can_view_devices) {
        return res.redirect('/panel');
    }
    
    const devices = db.prepare('SELECT * FROM devices WHERE client_id = ? ORDER BY is_online DESC, last_seen DESC').all(user.client_id);
    
    const rows = devices.map(d => `
        <tr>
            <td>
                <span style="display:inline-block;width:10px;height:10px;border-radius:50%;background:${d.is_online ? '#10b981' : '#666'};margin-right:8px"></span>
                <code style="background:#0f3460;padding:3px 10px;border-radius:4px;font-weight:bold">${d.device_code}</code>
            </td>
            <td>${d.device_name || '-'}</td>
            <td><small>${d.ip_address || '-'}</small></td>
            <td>${d.last_seen ? new Date(d.last_seen).toLocaleString() : '-'}</td>
            <td><span class="status ${d.is_online ? 'status-online' : 'status-offline'}">${d.is_online ? '🟢 Online' : '⚫ Offline'}</span></td>
        </tr>
    `).join('');
    
    res.send(`<!DOCTYPE html><html><head><title>Dispositivos - Vicviewer®</title>${userCSS}</head><body>
    <div class="container">${userHeader(user, 'devices')}
        <h2 style="margin-bottom:20px">💻 Mis Dispositivos</h2>
        <div class="card">
            ${devices.length ? `<table>
                <thead><tr><th>Código</th><th>Nombre</th><th>IP</th><th>Última actividad</th><th>Estado</th></tr></thead>
                <tbody>${rows}</tbody>
            </table>` : '<div class="empty">No tienes dispositivos registrados como servicio.</div>'}
        </div>
        <p style="color:#888;font-size:13px;margin-top:15px">
            ℹ️ Los dispositivos aparecen aquí cuando instalas Vicviewer® como servicio con un código fijo.
        </p>
    </div>
    <script>setTimeout(() => location.reload(), 30000);</script>
    </body></html>`);
});

app.get('/panel/history', userAuthMiddleware, (req, res) => {
    const user = req.user;
    
    if (!user.can_view_history) {
        return res.redirect('/panel');
    }
    
    const history = db.prepare(`SELECT * FROM connection_history WHERE client_id = ? ORDER BY created_at DESC LIMIT 50`).all(user.client_id);
    
    const actionLabels = {
        'host_started': '🟢 Host Iniciado',
        'service_started': '🔄 Servicio Iniciado',
        'viewer_connected': '👁️ Viewer Conectado',
        'viewer_connecting': '🔗 Viewer Conectando',
        'host_disconnected': '🔴 Desconectado'
    };
    
    const rows = history.map(h => `
        <tr>
            <td>${new Date(h.created_at).toLocaleString()}</td>
            <td><code>${h.access_code}</code></td>
            <td>${actionLabels[h.action] || h.action}</td>
            <td><small>${h.ip_address || '-'}</small></td>
        </tr>
    `).join('');
    
    res.send(`<!DOCTYPE html><html><head><title>Historial - Vicviewer®</title>${userCSS}</head><body>
    <div class="container">${userHeader(user, 'history')}
        <h2 style="margin-bottom:20px">📜 Mi Historial</h2>
        <div class="card">
            ${history.length ? `<table>
                <thead><tr><th>Fecha</th><th>Código</th><th>Acción</th><th>IP</th></tr></thead>
                <tbody>${rows}</tbody>
            </table>` : '<div class="empty">No hay registros de conexiones.</div>'}
        </div>
    </div>
    </body></html>`);
});

// ============== PÁGINA DE DESCARGA POR SUBDOMINIO ==============
// Esto requeriría un wildcard DNS, pero podemos manejarlo con query param por ahora
app.get('/download/:code', (req, res) => {
    const code = req.params.code.toUpperCase();
    const user = db.prepare('SELECT * FROM users WHERE company_code = ? AND status = ?').get(code, 'active');
    
    if (!user) {
        return res.status(404).send(`<!DOCTYPE html><html><head><title>No encontrado</title>${userCSS}</head><body>
        <div class="login-container"><div class="login-box">
            <h1>❌ Código no válido</h1>
            <p style="color:#888;text-align:center">El código de empresa no existe o no está activo.</p>
        </div></div></body></html>`);
    }
    
    res.send(`<!DOCTYPE html><html><head><title>Vicviewer® - ${user.company_name || user.name}</title>${userCSS}</head><body>
    <div class="login-container">
        <div class="download-box" style="max-width:500px">
            <h2>🖥️ Vicviewer®</h2>
            <p style="color:#888">Descarga el software de control remoto</p>
            <div class="code">${code}</div>
            <p style="color:#aaa;margin-bottom:20px">Código de empresa: <strong>${user.company_name || user.name}</strong></p>
            <a href="/api/download/Vicviewer${code}.exe" class="btn btn-primary" style="padding:15px 40px;font-size:16px">
                📥 Descargar Vicviewer${code}.exe
            </a>
            <p style="color:#666;font-size:12px;margin-top:20px">Windows 10/11 • 64 bits</p>
        </div>
    </div>
    </body></html>`);
});

// API para descargar exe con nombre personalizado
app.get('/api/download/:filename', (req, res) => {
    const filename = req.params.filename;
    // Servir el exe base con nombre personalizado
    const basePath = '/opt/vicviewer/dist/VicViewer.exe';
    const fs = require('fs');
    
    if (!fs.existsSync(basePath)) {
        return res.status(404).send('Archivo no encontrado. Contacte al administrador.');
    }
    
    res.setHeader('Content-Disposition', `attachment; filename="${filename}"`);
    res.setHeader('Content-Type', 'application/octet-stream');
    fs.createReadStream(basePath).pipe(res);
});

// ============== INICIAR SERVIDOR ==============

// ============== ENDPOINTS FREE MODE Y SERVICE PASSWORD ==============

app.post('/api/validate-account', (req, res) => {
    const { companyCode, diskSerial } = req.body;
    
    if (!diskSerial) {
        return res.json({
            allowed: false,
            isPaid: false,
            error: 'diskSerial requerido'
        });
    }
    
    const status = checkFreeModeStatus(diskSerial, companyCode);
    res.json(status);
});

app.post('/api/validate-service-password', (req, res) => {
    const { companyCode, servicePassword } = req.body;
    
    if (!companyCode || !servicePassword) {
        return res.json({
            valid: false,
            error: 'Parámetros incompletos',
            message: 'Se requiere código de empresa y contraseña'
        });
    }
    
    const user = db.prepare(`
        SELECT id, service_password, name, company_name, status 
        FROM users 
        WHERE company_code = ?
    `).get(companyCode);
    
    if (!user) {
        return res.json({ valid: false, error: 'not_found', message: 'Código de empresa no encontrado' });
    }
    
    if (user.status !== 'active') {
        return res.json({ valid: false, error: 'inactive', message: 'La cuenta no está activa' });
    }
    
    if (!user.service_password) {
        return res.json({ valid: false, error: 'no_password', message: 'No se ha generado contraseña de servicio' });
    }
    
    if (user.service_password.toUpperCase() !== servicePassword.toUpperCase()) {
        return res.json({ valid: false, error: 'invalid', message: 'Contraseña incorrecta' });
    }
    
    log('info', `🔑 Contraseña de servicio validada: ${companyCode}`);
    res.json({ valid: true, userName: user.name, companyName: user.company_name });
});

app.post('/api/end-free-session', (req, res) => {
    const { diskSerial } = req.body;
    if (!diskSerial) return res.json({ success: false, error: 'diskSerial requerido' });
    endFreeSession(diskSerial);
    res.json({ success: true });
});

app.post('/api/user/generate-service-password', userAuthMiddleware, (req, res) => {
    const newPassword = generateServicePassword();
    db.prepare(`UPDATE users SET service_password = ? WHERE id = ?`).run(newPassword, req.user.id);
    log('info', `🔑 Contraseña de servicio generada para usuario ${req.user.id}`);
    res.json({ success: true, servicePassword: newPassword });
});

app.get('/api/user/service-password', userAuthMiddleware, (req, res) => {
    const user = db.prepare(`SELECT service_password FROM users WHERE id = ?`).get(req.user.id);
    res.json({ servicePassword: user?.service_password || null });
});

app.listen(PORT, '0.0.0.0', () => {
    log('info', `
╔═══════════════════════════════════════════╗
║     🖥️  Vicviewer® Matchmaker v1.2       ║
╠═══════════════════════════════════════════╣
║  API:    http://0.0.0.0:${PORT}/v1/sessions   ║
║  Admin:  http://0.0.0.0:${PORT}/admin         ║
║  Nivel:  ${LOG_LEVEL.toUpperCase().padEnd(30)}║
╚═══════════════════════════════════════════╝
    `);
});

// Limpieza al cerrar
process.on('SIGTERM', () => {
    log('info', '🛑 Señal SIGTERM recibida, cerrando...');
    // Marcar todos los dispositivos como offline
    db.prepare('UPDATE devices SET is_online = 0').run();
    sessions.clear();
    process.exit(0);
});

process.on('SIGINT', () => {
    log('info', '🛑 Señal SIGINT recibida, cerrando...');
    db.prepare('UPDATE devices SET is_online = 0').run();
    sessions.clear();
    process.exit(0);
});
