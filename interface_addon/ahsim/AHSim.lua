-- Talks to the AuctionSim server module. Every message, both ways, is
-- "MSGTYPE\tfield\tfield...". Client -> server goes out as a self-whisper.
-- AHSim.OP below mirrors the Msg namespace in AuctionSimAddonBridge.cpp.

local tconcat = table.concat
local sfind = string.find
local ssub = string.sub

AHSim = AHSim or {}
AHSim.PREFIX = "AHSIM"
AHSim.authorized = false
AHSim.handlers = {}
AHSim.playerName = nil  -- cached at PLAYER_LOGIN; the self-whisper target

-- The one place message-type strings live on the client. Values equal keys and
-- must match AuctionSimAddonBridge.cpp's Msg namespace.
AHSim.OP = {
    -- client -> server
    WHOAMI = "WHOAMI",
    GETCONFIG = "GETCONFIG",
    SETCONFIG = "SETCONFIG",
    SAVECONFIG = "SAVECONFIG",
    SCAN = "SCAN",
    DELETE = "DELETE",
    TEST = "TEST",
    CLEANOVERCAP = "CLEANOVERCAP",
    SHOWQUEUE = "SHOWQUEUE",
    SETBOTCHAR = "SETBOTCHAR",
    -- server -> client
    ERROR = "ERROR",
    CONFIG = "CONFIG",
    CONFIGSAVED = "CONFIGSAVED",
    SCANRESULT = "SCANRESULT",
    DELETERESULT = "DELETERESULT",
    TESTRESULT = "TESTRESULT",
    TESTDONE = "TESTDONE",
    QUEUEINFO = "QUEUEINFO",
    CLEANRESULT = "CLEANRESULT",
    SETBOTCHARRESULT = "SETBOTCHARRESULT",
}

function AHSim:RegisterHandler(msgType, fn)
    self.handlers[msgType] = fn
end

function AHSim:Send(...)
    local target = self.playerName or UnitName("player")
    SendAddonMessage(self.PREFIX, tconcat({...}, "\t"), "WHISPER", target)
end

local function SplitTabs(str)
    local result = {}
    local count = 0
    local from = 1
    while true do
        local tabPos = sfind(str, "\t", from, true)
        if not tabPos then
            count = count + 1
            result[count] = ssub(str, from)
            break
        end
        count = count + 1
        result[count] = ssub(str, from, tabPos - 1)
        from = tabPos + 1
    end
    return result
end

function AHSim:Dispatch(message)
    local fields = SplitTabs(message)
    local msgType = fields[1]
    if not msgType then
        return
    end

    local handler = self.handlers[msgType]
    if handler then
        handler(unpack(fields, 2))
    end
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("PLAYER_LOGIN")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:SetScript("OnEvent", function(self, event, ...)
    if event == "PLAYER_LOGIN" then
        AHSim.playerName = UnitName("player")
        -- server only answers WHOAMI for GMs; the reply gates window creation
        AHSim:Send(AHSim.OP.WHOAMI)
    elseif event == "CHAT_MSG_ADDON" then
        local prefix, message = ...
        if prefix == AHSim.PREFIX then
            AHSim:Dispatch(message)
        end
    end
end)
