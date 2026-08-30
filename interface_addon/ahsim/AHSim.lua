-- Talks to the AuctionSim server module. Every message, both ways, is
-- "MSGTYPE\tfield\tfield...". Client -> server goes out as a self-whisper.
-- AuctionSimAddonBridge.cpp has the message list.

AHSim = AHSim or {}
AHSim.PREFIX = "AHSIM"
AHSim.authorized = false
AHSim.handlers = {}

function AHSim:RegisterHandler(msgType, fn)
    self.handlers[msgType] = fn
end

function AHSim:Send(...)
    local payload = table.concat({...}, "\t")
    SendAddonMessage(self.PREFIX, payload, "WHISPER", UnitName("player"))
end

local function SplitTabs(str)
    local result = {}
    local from = 1
    while true do
        local tabPos = string.find(str, "\t", from, true)
        if not tabPos then
            table.insert(result, string.sub(str, from))
            break
        end
        table.insert(result, string.sub(str, from, tabPos - 1))
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
        -- server only answers WHOAMI for GMs; the reply gates window creation
        AHSim:Send("WHOAMI")
    elseif event == "CHAT_MSG_ADDON" then
        local prefix, message, channel, sender = ...
        if prefix == AHSim.PREFIX then
            AHSim:Dispatch(message)
        end
    end
end)
