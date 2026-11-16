-- AgentBridge: lets a test harness run Lua inside a test copy of the game, without input or screen access.
-- Only active when TOW2_SANDBOX_DIR is set (test copies made by gamepool). Protocol, one command at a time:
--   harness writes <sandbox>\bridge\cmd.lua (first line "-- id: <n>"), atomically by rename
--   bridge runs it once on the game thread inside xpcall, prints go to the reply
--   bridge writes <sandbox>\bridge\out.txt ("id: <n>", "ok: true|false", printed lines, return value or error)
-- <sandbox>\bridge\heartbeat.txt is rewritten every 2 s (a readiness and liveness probe).
local root = os.getenv("TOW2_SANDBOX_DIR")
if not root or root == "" then
    return
end
local dir = root .. "\\bridge\\"

local function readall(path)
    local f = io.open(path, "rb")
    if not f then
        return nil
    end
    local s = f:read("a")
    f:close()
    return s
end

local function writeall(path, text)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "wb")
    if not f then
        return
    end
    f:write(text)
    f:close()
    os.remove(path)
    os.rename(tmp, path)
end

local function run(id, code)
    local buf = {}
    local env = setmetatable({
        print = function(...)
            local t = table.pack(...)
            for i = 1, t.n do
                t[i] = tostring(t[i])
            end
            buf[#buf + 1] = table.concat(t, "\t")
        end,
    }, { __index = _G })
    local fn, err = load(code, "=cmd" .. id, "t", env)
    local ok, res
    if fn then
        ok, res = xpcall(fn, debug.traceback)
    else
        ok, res = false, err
    end
    if res ~= nil then
        buf[#buf + 1] = (ok and "return: " or "error: ") .. tostring(res)
    end
    writeall(dir .. "out.txt", "id: " .. id .. "\nok: " .. tostring(ok) .. "\n" .. table.concat(buf, "\n") .. "\n")
end

local ticks = 0
LoopAsync(250, function()
    ticks = ticks + 1
    local code = readall(dir .. "cmd.lua")
    if code then
        os.remove(dir .. "cmd.lua")
        local id = code:match("^%-%- id: (%d+)") or "0"
        ExecuteInGameThread(function()
            run(id, code)
        end)
    end
    if ticks % 8 == 0 then
        writeall(dir .. "heartbeat.txt", tostring(os.time()) .. " " .. ticks .. "\n")
    end
    return false
end)
print("[AgentBridge] listening in " .. dir .. "\n")
