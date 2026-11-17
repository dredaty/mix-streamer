#include "StreamHub.h"
#include "Sha1.h"
#include "BinaryData.h"
#include <deque>

#if ! JUCE_WINDOWS
 #include <sys/socket.h>
 #include <cerrno>
#endif

namespace
{
    juce::uint32 nowMs() noexcept { return juce::Time::getMillisecondCounter(); }

    /** A write to a socket the peer already closed must never raise SIGPIPE inside the
        host process (it would kill the DAW). We must not touch the host's signal handlers,
        so: macOS → SO_NOSIGPIPE per socket, Linux → MSG_NOSIGNAL, Windows has no SIGPIPE. */
    void preventSigPipe (juce::StreamingSocket& s)
    {
       #if JUCE_MAC
        int one = 1;
        setsockopt (s.getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
       #else
        juce::ignoreUnused (s);
       #endif
    }

    bool writeAll (juce::StreamingSocket& s, const void* data, size_t len)
    {
       #if JUCE_WINDOWS
        return s.write (data, (int) len) == (int) len;
       #else
        const int fd = s.getRawSocketHandle();
        if (fd < 0) return false;
        auto* p = static_cast<const char*> (data);
        while (len > 0)
        {
           #if JUCE_LINUX || JUCE_BSD
            const auto n = ::send (fd, p, len, MSG_NOSIGNAL);
           #else
            const auto n = ::send (fd, p, len, 0);
           #endif
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n;
            len -= (size_t) n;
        }
        return true;
       #endif
    }

    /** Server → client frame (never masked). */
    void encodeFrame (juce::MemoryBlock& out, juce::uint8 opcode, const void* data, size_t len)
    {
        juce::uint8 hdr[10];
        size_t h = 0;
        hdr[h++] = (juce::uint8) (0x80 | opcode);
        if (len < 126)            { hdr[h++] = (juce::uint8) len; }
        else if (len <= 0xFFFF)   { hdr[h++] = 126; hdr[h++] = (juce::uint8) (len >> 8); hdr[h++] = (juce::uint8) len; }
        else                      { hdr[h++] = 127; for (int i = 7; i >= 0; --i) hdr[h++] = (juce::uint8) ((juce::uint64) len >> (8 * i)); }
        out.setSize (h + len, false);
        memcpy (out.getData(), hdr, h);
        if (len > 0) memcpy (static_cast<char*> (out.getData()) + h, data, len);
    }

    juce::String wsAcceptKey (const juce::String& clientKey)
    {
        const auto s = (clientKey.trim() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toStdString();
        const auto digest = sha1 (s.data(), s.size());
        return juce::Base64::toBase64 (digest.data(), digest.size());
    }

    juce::String tunnelLogPath()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("samov-streamer-cloudflared.log").getFullPathName();
    }
}

//==============================================================================
/** One browser connected to /listen. Own thread, bounded queue, drops on backlog. */
class ListenerConnection : public juce::Thread
{
public:
    explicit ListenerConnection (std::unique_ptr<juce::StreamingSocket> s)
        : juce::Thread ("MixStreamer Listener"), socket (std::move (s)) {}

    ~ListenerConnection() override
    {
        signalThreadShouldExit();
        notify();
        socket->close();
        stopThread (2000);
    }

    void enqueue (Packet p, bool isText)
    {
        {
            const juce::ScopedLock sl (qlock);
            queue.push_back ({ std::move (p), isText });
            while (queue.size() > maxQueued) // slow client: keep newest audio
                queue.pop_front();
        }
        notify();
    }

    bool isFinished() const noexcept { return finished.load(); }

private:
    struct Item { Packet data; bool text; };
    static constexpr size_t maxQueued = 150;   // ~1.5 s of 10 ms packets

    void run() override
    {
        juce::MemoryBlock frame, rx;
        size_t rxUsed = 0;
        rx.setSize (8192);
        auto lastPing = nowMs();

        while (! threadShouldExit())
        {
            std::deque<Item> local;
            {
                const juce::ScopedLock sl (qlock);
                local.swap (queue);
            }
            for (auto& it : local)
            {
                encodeFrame (frame, it.text ? 0x1 : 0x2, it.data->getData(), it.data->getSize());
                if (! writeAll (frame)) { finished = true; return; }
            }

            // keep-alive through Cloudflare (idle WS get closed after ~100 s)
            if (nowMs() - lastPing > 20000)
            {
                lastPing = nowMs();
                encodeFrame (frame, 0x9, nullptr, 0);
                if (! writeAll (frame)) { finished = true; return; }
            }

            // incoming: close / ping / ignore everything else
            while (socket->waitUntilReady (true, 0) == 1)
            {
                if (rxUsed + 1024 > rx.getSize()) { finished = true; return; } // nobody sends us big data
                const int n = socket->read (static_cast<char*> (rx.getData()) + rxUsed, 1024, false);
                if (n <= 0) { finished = true; return; }
                rxUsed += (size_t) n;
                if (! parseIncoming (rx, rxUsed, frame)) { finished = true; return; }
            }

            wait (20);
        }
        finished = true;
    }

    bool writeAll (const juce::MemoryBlock& b)
    {
        return ::writeAll (*socket, b.getData(), b.getSize());
    }

    /** Returns false if the connection should be closed. */
    bool parseIncoming (juce::MemoryBlock& rx, size_t& used, juce::MemoryBlock& scratch)
    {
        auto* buf = static_cast<juce::uint8*> (rx.getData());
        size_t off = 0;
        while (used - off >= 2)
        {
            const auto* f = buf + off;
            const juce::uint8 opcode = f[0] & 0x0F;
            size_t len = f[1] & 0x7F, hdr = 2;
            if (len == 126) { if (used - off < 4) break; len = ((size_t) f[2] << 8) | f[3]; hdr = 4; }
            else if (len == 127) return false;
            if (f[1] & 0x80) hdr += 4;
            if (used - off < hdr + len) break;

            auto* payload = buf + off + hdr;
            if (f[1] & 0x80)
                for (size_t i = 0; i < len; ++i) payload[i] ^= f[hdr - 4 + (i & 3)];

            if (opcode == 0x8) return false;
            if (opcode == 0x9)
            {
                encodeFrame (scratch, 0xA, payload, len);
                if (! writeAll (scratch)) return false;
            }
            off += hdr + len;
        }
        memmove (buf, buf + off, used - off);
        used -= off;
        return true;
    }

    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection qlock;
    std::deque<Item> queue;
    std::atomic<bool> finished { false };
};

//==============================================================================
/** Runs `cloudflared tunnel --url http://127.0.0.1:<port>` and parses the public URL. */
class CloudflaredTunnel : public juce::Thread
{
public:
    explicit CloudflaredTunnel (int localPort) : juce::Thread ("MixStreamer Tunnel"), port (localPort) {}
    ~CloudflaredTunnel() override { stop(); }

    void stop()
    {
        signalThreadShouldExit();
        notify();
        {
            const juce::ScopedLock sl (lock);
            if (proc != nullptr) proc->kill();
        }
        stopThread (5000);
    }

    juce::String getUrl() const     { const juce::ScopedLock sl (lock); return url; }
    TunnelState getState() const    { return state.load(); }

    static juce::File findBinary()
    {
       #if JUCE_WINDOWS
        const juce::String exe = "cloudflared.exe";
       #else
        const juce::String exe = "cloudflared";
       #endif
        juce::Array<juce::File> candidates;

        if (auto env = juce::SystemStats::getEnvironmentVariable ("MIXSTREAMER_CLOUDFLARED", {}); env.isNotEmpty())
            candidates.add (juce::File (env));

        // 1) bundled inside the plug-in: <bundle>/Contents/Resources/cloudflared[.exe]
        const auto module = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        candidates.add (module.getParentDirectory().getParentDirectory().getChildFile ("Resources").getChildFile (exe));

        // 2) usual install locations (winget / Homebrew)
       #if JUCE_WINDOWS
        candidates.add (juce::File ("C:\\Program Files (x86)\\cloudflared\\cloudflared.exe"));
        candidates.add (juce::File ("C:\\Program Files\\cloudflared\\cloudflared.exe"));
        const auto local = juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {});
        if (local.isNotEmpty())
            candidates.add (juce::File (local).getChildFile ("Microsoft\\WinGet\\Links\\cloudflared.exe"));
       #else
        candidates.add (juce::File ("/opt/homebrew/bin/cloudflared"));
        candidates.add (juce::File ("/usr/local/bin/cloudflared"));
       #endif

        // 3) PATH
        auto path = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}),
                                                  #if JUCE_WINDOWS
                                                   ";",
                                                  #else
                                                   ":",
                                                  #endif
                                                   "\"");
        for (auto& dir : path)
            if (dir.isNotEmpty() && juce::File::isAbsolutePath (dir))
                candidates.add (juce::File (dir).getChildFile (exe));

        for (auto& f : candidates)
            if (f.existsAsFile())
                return f;
        return {};
    }

private:
    void run() override
    {
        int backoffMs = 2000;
        juce::FileOutputStream* log = nullptr;
        std::unique_ptr<juce::FileOutputStream> logOwner;
        {
            juce::File lf (tunnelLogPath());
            lf.deleteFile();
            logOwner = std::make_unique<juce::FileOutputStream> (lf);
            if (logOwner->openedOk()) log = logOwner.get();
        }

        while (! threadShouldExit())
        {
            const auto bin = findBinary();
            if (bin == juce::File())
            {
                state = TunnelState::missingBinary;
                wait (5000);
                continue;
            }
           #if ! JUCE_WINDOWS
            bin.setExecutePermission (true);  // may fail on read-only locations; harmless
           #endif

            state = TunnelState::starting;
            juce::ChildProcess child;
            const juce::StringArray args { bin.getFullPathName(), "tunnel", "--no-autoupdate",
                                           "--url", "http://127.0.0.1:" + juce::String (port) };
            if (log) log->writeText ("$ " + args.joinIntoString (" ") + "\n", false, false, nullptr);

            if (! child.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            {
                state = TunnelState::error;
                wait (backoffMs);
                backoffMs = juce::jmin (backoffMs * 2, 60000);
                continue;
            }
            {
                const juce::ScopedLock sl (lock);
                proc = &child;
            }

            juce::String line;
            for (;;)
            {
                char c;
                // NB: ChildProcess::readProcessOutput blocks until the requested amount
                // arrives, so read byte-wise to see log lines immediately.
                if (child.readProcessOutput (&c, 1) != 1)
                    break;
                if (c != '\n') { if (line.length() < 2000) line += c; continue; }

                if (log) { log->writeText (line + "\n", false, false, nullptr); log->flush(); }
                const auto found = extractUrl (line);
                if (found.isNotEmpty())
                {
                    { const juce::ScopedLock sl (lock); url = found; }
                    state = TunnelState::online;
                    backoffMs = 2000;
                }
                line.clear();
            }

            {
                const juce::ScopedLock sl (lock);
                proc = nullptr;
                url.clear();
            }
            child.kill();
            if (threadShouldExit()) break;

            state = TunnelState::error;
            if (log) log->writeText ("[cloudflared exited, restarting]\n", false, false, nullptr);
            wait (backoffMs);
            backoffMs = juce::jmin (backoffMs * 2, 60000);
        }
        state = TunnelState::idle;
    }

    static juce::String extractUrl (const juce::String& line)
    {
        const int end = line.indexOfIgnoreCase (".trycloudflare.com");
        if (end < 0) return {};
        const int start = line.substring (0, end).lastIndexOf ("https://");
        if (start < 0) return {};
        const auto host = line.substring (start + 8, end);
        if (host.isEmpty() || ! host.containsChar ('-')
            || ! host.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"))
            return {};
        return line.substring (start, end + 18);
    }

    const int port;
    juce::CriticalSection lock;
    juce::ChildProcess* proc = nullptr;
    juce::String url;
    std::atomic<TunnelState> state { TunnelState::idle };
};

//==============================================================================
StreamHub::StreamHub() : juce::Thread ("MixStreamer HTTP") {}

StreamHub::~StreamHub()
{
    if (tunnel) tunnel->stop();
    stopThread (3000);   // accept loop polls every 200 ms
    server.close();
    const juce::ScopedLock sl (lock);
    listeners.clear();
}

void StreamHub::activate()
{
    const juce::ScopedLock sl (lock);
    if (active.load()) return;

    if (! server.createListener (0, "127.0.0.1"))
        return;
    port = server.getBoundPort();
    startThread();

    if (juce::SystemStats::getEnvironmentVariable ("MIXSTREAMER_NO_TUNNEL", {}) != "1")
    {
        tunnel = std::make_unique<CloudflaredTunnel> (port.load());
        tunnel->startThread();
    }
    active = true;
}

bool StreamHub::claimSource (const void* owner) noexcept
{
    const void* expected = nullptr;
    return source.compare_exchange_strong (expected, owner) || expected == owner;
}

void StreamHub::releaseSource (const void* owner) noexcept
{
    const void* expected = owner;
    source.compare_exchange_strong (expected, nullptr);
}

juce::String StreamHub::getPublicUrl() const
{
    return tunnel ? tunnel->getUrl() : juce::String();
}

TunnelState StreamHub::getTunnelState() const
{
    if (! active.load()) return TunnelState::idle;
    return tunnel ? tunnel->getState() : TunnelState::disabled;
}

bool StreamHub::isSourceLive() const noexcept
{
    const auto last = lastPacketMs.load();
    return last != 0 && nowMs() - last < 1000;
}

void StreamHub::publish (const void* owner, Packet packet, juce::uint32 sampleRate, int bitDepth)
{
    if (! isSource (owner)) return;

    const bool formatChanged = fmtRate.exchange (sampleRate) != sampleRate || fmtBits.exchange (bitDepth) != bitDepth;
    const bool wasLive = isSourceLive();
    lastPacketMs = juce::jmax ((juce::uint32) 1, nowMs());

    const juce::ScopedLock sl (lock);
    for (auto* l : listeners)
        l->enqueue (packet, false);
    if (formatChanged || ! wasLive)
    {
        const auto json = statusJson();
        auto msg = std::make_shared<const juce::MemoryBlock> (json.toRawUTF8(), json.getNumBytesAsUTF8());
        for (auto* l : listeners) l->enqueue (msg, true);
    }
}

juce::String StreamHub::statusJson() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "status");
    o->setProperty ("sourceConnected", isSourceLive());
    o->setProperty ("listeners", listenerCount.load());
    if (fmtRate.load() != 0)
    {
        auto* f = new juce::DynamicObject();
        f->setProperty ("sampleRate", (int) fmtRate.load());
        f->setProperty ("bitDepth", fmtBits.load());
        f->setProperty ("channels", 2);
        o->setProperty ("format", juce::var (f));
    }
    else
        o->setProperty ("format", juce::var());
    o->setProperty ("publicUrl", getPublicUrl());
    return juce::JSON::toString (juce::var (o), true);
}

void StreamHub::broadcastStatus()
{
    const auto json = statusJson();
    auto msg = std::make_shared<const juce::MemoryBlock> (json.toRawUTF8(), json.getNumBytesAsUTF8());
    const juce::ScopedLock sl (lock);
    for (auto* l : listeners) l->enqueue (msg, true);
}

void StreamHub::run()
{
    bool lastLive = false;
    while (! threadShouldExit())
    {
        if (server.waitUntilReady (true, 200) == 1)
            if (auto* c = server.waitForNextConnection())
                handleConnection (std::unique_ptr<juce::StreamingSocket> (c));

        // reap closed listeners
        bool changed = false;
        {
            const juce::ScopedLock sl (lock);
            for (int i = listeners.size(); --i >= 0;)
                if (listeners[i]->isFinished()) { listeners.remove (i); changed = true; }
            listenerCount = listeners.size();
        }
        const bool live = isSourceLive();
        if (changed || live != lastLive) broadcastStatus();
        lastLive = live;
    }
}

void StreamHub::sendHttp (juce::StreamingSocket& s, const juce::String& status, const juce::String& type,
                          const void* body, size_t size)
{
    const auto head = "HTTP/1.1 " + status + "\r\n"
                      "Content-Type: " + type + "\r\n"
                      "Content-Length: " + juce::String ((juce::int64) size) + "\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Connection: close\r\n\r\n";
    writeAll (s, head.toRawUTF8(), head.getNumBytesAsUTF8());
    if (size > 0) writeAll (s, body, size);
}

void StreamHub::handleConnection (std::unique_ptr<juce::StreamingSocket> sock)
{
    preventSigPipe (*sock);

    // read request head (max 16 KB, 2 s)
    juce::MemoryBlock req;
    const auto deadline = nowMs() + 2000;
    juce::String head;
    char buf[1024];
    while (! head.contains ("\r\n\r\n"))
    {
        const int remaining = (int) (deadline - nowMs());
        if (remaining <= 0 || head.length() > 16384 || sock->waitUntilReady (true, remaining) != 1) return;
        const int n = sock->read (buf, sizeof (buf), false);
        if (n <= 0) return;
        head += juce::String::fromUTF8 (buf, n);
    }

    auto lines = juce::StringArray::fromLines (head.upToFirstOccurrenceOf ("\r\n\r\n", false, false));
    auto requestLine = juce::StringArray::fromTokens (lines[0], " ", {});
    if (requestLine.size() < 2 || requestLine[0] != "GET")
    {
        sendHttp (*sock, "405 Method Not Allowed", "text/plain", "", 0);
        return;
    }
    const auto path = requestLine[1].upToFirstOccurrenceOf ("?", false, false);

    juce::StringPairArray headers (false);
    for (int i = 1; i < lines.size(); ++i)
        headers.set (lines[i].upToFirstOccurrenceOf (":", false, false).trim().toLowerCase(),
                     lines[i].fromFirstOccurrenceOf (":", false, false).trim());

    if (path == "/listen")
    {
        const auto key = headers["sec-websocket-key"];
        if (! headers["upgrade"].equalsIgnoreCase ("websocket") || key.isEmpty())
        {
            sendHttp (*sock, "400 Bad Request", "text/plain", "", 0);
            return;
        }
        const auto resp = "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " + wsAcceptKey (key) + "\r\n\r\n";
        writeAll (*sock, resp.toRawUTF8(), resp.getNumBytesAsUTF8());

        auto* l = new ListenerConnection (std::move (sock));
        {
            const juce::ScopedLock sl (lock);
            listeners.add (l);
            listenerCount = listeners.size();
        }
        l->startThread();
        broadcastStatus();
        return;
    }

    if (path == "/api/status")
    {
        const auto json = statusJson();
        sendHttp (*sock, "200 OK", "application/json", json.toRawUTF8(), json.getNumBytesAsUTF8());
        return;
    }

    struct File { const char* path; const char* data; int size; const char* type; };
    const File files[] = {
        { "/",           BinaryData::index_html, BinaryData::index_htmlSize, "text/html; charset=utf-8" },
        { "/index.html", BinaryData::index_html, BinaryData::index_htmlSize, "text/html; charset=utf-8" },
        { "/app.js",     BinaryData::app_js,     BinaryData::app_jsSize,     "text/javascript; charset=utf-8" },
        { "/worklet.js", BinaryData::worklet_js, BinaryData::worklet_jsSize, "text/javascript; charset=utf-8" },
        { "/logo.svg",   BinaryData::logo_svg,   BinaryData::logo_svgSize,   "image/svg+xml" },
    };
    for (auto& f : files)
        if (path == f.path)
        {
            sendHttp (*sock, "200 OK", f.type, f.data, (size_t) f.size);
            return;
        }

    sendHttp (*sock, "404 Not Found", "text/plain", "Not found", 9);
}
