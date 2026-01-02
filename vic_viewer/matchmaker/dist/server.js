"use strict";
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const express_1 = __importDefault(require("express"));
const cors_1 = __importDefault(require("cors"));
const ws_1 = require("ws");
const SESSION_TTL_MS = Number(process.env.SESSION_TTL_MS ?? 300000); // 5 min default
const CLEANUP_INTERVAL_MS = Number(process.env.CLEANUP_INTERVAL_MS ?? 60000);
const PORT = Number(process.env.PORT ?? 8080);
const app = (0, express_1.default)();
app.use((0, cors_1.default)());
app.use(express_1.default.json({ limit: "1mb" }));
const sessions = new Map();
function normalizeCode(code) {
    return code.trim().toUpperCase();
}
function generateCode() {
    const chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    let code = "";
    for (let i = 0; i < 6; i++) {
        code += chars[Math.floor(Math.random() * chars.length)];
    }
    return code;
}
function cleanupSessions() {
    const threshold = Date.now() - SESSION_TTL_MS;
    let removed = 0;
    for (const [code, session] of sessions.entries()) {
        if (session.lastAccessAt < threshold) {
            sessions.delete(code);
            broadcastUpdate({ type: "expired", code });
            removed++;
        }
    }
    if (removed > 0) {
        console.log(`[cleanup] Removed ${removed} expired sessions. Active: ${sessions.size}`);
    }
}
setInterval(cleanupSessions, CLEANUP_INTERVAL_MS).unref?.();
// ============================================================================
// POST /register - Host registra su offer y ICE candidates
// Body: { code?, offer, iceCandidates, iceServers }
// ============================================================================
app.post("/register", (req, res) => {
    cleanupSessions();
    const { code, offer, iceCandidates, iceServers } = req.body ?? {};
    if (!offer || !offer.sdp) {
        res.status(400).json({ error: "Missing offer.sdp" });
        return;
    }
    // Generar código si no se proporciona
    let sessionCode = code ? normalizeCode(code) : generateCode();
    // Si el código ya existe, generar uno nuevo
    while (sessions.has(sessionCode) && !code) {
        sessionCode = generateCode();
    }
    const session = {
        code: sessionCode,
        offer: {
            type: offer.type || "offer",
            sdp: offer.sdp
        },
        iceCandidates: Array.isArray(iceCandidates) ? iceCandidates : [],
        iceServers: Array.isArray(iceServers) ? iceServers : [],
        createdAt: Date.now(),
        lastAccessAt: Date.now()
    };
    sessions.set(sessionCode, session);
    broadcastUpdate({ type: "registered", session: { code: sessionCode } });
    console.log(`[register] Host registered with code: ${sessionCode}`);
    res.json({ code: sessionCode });
});
// ============================================================================
// GET /resolve?code=XXX - Viewer obtiene offer del host
// Returns: { code, offer, iceCandidates, iceServers }
// ============================================================================
app.get("/resolve", (req, res) => {
    cleanupSessions();
    const code = normalizeCode(String(req.query.code ?? ""));
    if (!code) {
        res.status(400).json({ error: "Missing code parameter" });
        return;
    }
    const session = sessions.get(code);
    if (!session) {
        res.status(404).json({ error: "Code not found" });
        return;
    }
    session.lastAccessAt = Date.now();
    broadcastUpdate({ type: "resolved", code });
    console.log(`[resolve] Viewer resolved code: ${code}`);
    res.json({
        code: session.code,
        offer: session.offer,
        iceCandidates: session.iceCandidates,
        iceServers: session.iceServers
    });
});
// ============================================================================
// POST /answer - Viewer envía su answer al host
// Body: { code, answer, iceCandidates }
// ============================================================================
app.post("/answer", (req, res) => {
    const { code, answer, iceCandidates } = req.body ?? {};
    if (!code) {
        res.status(400).json({ error: "Missing code" });
        return;
    }
    if (!answer || !answer.sdp) {
        res.status(400).json({ error: "Missing answer.sdp" });
        return;
    }
    const normalizedCode = normalizeCode(code);
    const session = sessions.get(normalizedCode);
    if (!session) {
        res.status(404).json({ error: "Code not found" });
        return;
    }
    session.answer = {
        type: answer.type || "answer",
        sdp: answer.sdp
    };
    session.answerIceCandidates = Array.isArray(iceCandidates) ? iceCandidates : [];
    session.lastAccessAt = Date.now();
    broadcastUpdate({ type: "answered", code: normalizedCode });
    console.log(`[answer] Viewer submitted answer for code: ${normalizedCode}`);
    res.json({ success: true });
});
// ============================================================================
// GET /answer?code=XXX - Host obtiene answer del viewer
// Returns: { answer, iceCandidates } o 404 si no hay answer aún
// ============================================================================
app.get("/answer", (req, res) => {
    const code = normalizeCode(String(req.query.code ?? ""));
    if (!code) {
        res.status(400).json({ error: "Missing code parameter" });
        return;
    }
    const session = sessions.get(code);
    if (!session) {
        res.status(404).json({ error: "Code not found" });
        return;
    }
    if (!session.answer) {
        // No hay answer todavía - el viewer aún no ha respondido
        res.status(404).json({ error: "No answer yet" });
        return;
    }
    session.lastAccessAt = Date.now();
    console.log(`[answer] Host fetched answer for code: ${code}`);
    res.json({
        answer: session.answer,
        iceCandidates: session.answerIceCandidates || []
    });
});
// ============================================================================
// DELETE /register/:code - Eliminar sesión
// ============================================================================
app.delete("/register/:code", (req, res) => {
    const code = normalizeCode(req.params.code ?? "");
    if (sessions.delete(code)) {
        broadcastUpdate({ type: "removed", code });
        console.log(`[delete] Session removed: ${code}`);
    }
    res.sendStatus(204);
});
// ============================================================================
// GET /health - Health check
// ============================================================================
app.get("/health", (_req, res) => {
    cleanupSessions();
    res.json({
        status: "ok",
        activeSessions: sessions.size,
        ttlMs: SESSION_TTL_MS,
        uptime: process.uptime()
    });
});
// ============================================================================
// GET /sessions - Debug: listar sesiones activas
// ============================================================================
app.get("/sessions", (_req, res) => {
    cleanupSessions();
    const list = Array.from(sessions.values()).map(s => ({
        code: s.code,
        hasOffer: !!s.offer,
        hasAnswer: !!s.answer,
        createdAt: new Date(s.createdAt).toISOString(),
        lastAccessAt: new Date(s.lastAccessAt).toISOString(),
        candidateCount: s.iceCandidates.length
    }));
    res.json({ count: list.length, sessions: list });
});
// ============================================================================
// WebSocket para notificaciones en tiempo real
// ============================================================================
const server = app.listen(PORT, () => {
    console.log(`VicViewer Matchmaker listening on port ${PORT}`);
    console.log(`  - Session TTL: ${SESSION_TTL_MS / 1000}s`);
    console.log(`  - Cleanup interval: ${CLEANUP_INTERVAL_MS / 1000}s`);
});
const wss = new ws_1.WebSocketServer({ server });
function broadcastUpdate(message) {
    const payload = JSON.stringify(message);
    wss.clients.forEach((client) => {
        if (client.readyState === ws_1.WebSocket.OPEN) {
            client.send(payload);
        }
    });
}
wss.on("connection", (socket) => {
    console.log("[ws] Client connected");
    // Enviar snapshot de sesiones activas
    const snapshot = Array.from(sessions.keys());
    socket.send(JSON.stringify({ type: "snapshot", codes: snapshot }));
    socket.on("close", () => {
        console.log("[ws] Client disconnected");
    });
});
// Graceful shutdown
process.on("SIGTERM", () => {
    console.log("SIGTERM received, shutting down...");
    wss.close();
    server.close(() => {
        process.exit(0);
    });
});
