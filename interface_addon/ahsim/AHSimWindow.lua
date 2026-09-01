-- /auctionsim slash command. GM-only: non-GMs get no WHOAMI reply, so no window,
-- and any request they send is dropped server-side anyway.

-- Skip re-fetching the ~120-value config grid if we pulled it this recently.
local CONFIG_FETCH_TTL = 10

function AHSim.RequestConfig(force)
    if force or not AHSim.lastConfigFetch or (GetTime() - AHSim.lastConfigFetch) > CONFIG_FETCH_TTL then
        AHSim:Send(AHSim.OP.GETCONFIG)
        AHSim.lastConfigFetch = GetTime()
    end
end

local function OpenWindow()
    if not AHSim.authorized then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffd100AuctionSim:|r you are not authorized to use the Bot Manager.")
        return
    end

    -- usually already built at login; covers WHOAMI still being in flight
    if not AHSimFrame and AHSim.BuildWindow then
        AHSim.BuildWindow()
    end
    if not AHSimFrame then
        return
    end

    AHSimFrame:Show()
    AHSimFrame:Raise()
    AHSim.RequestConfig(false)  -- refresh in case it changed elsewhere (debounced)
end

local function ToggleWindow()
    if AHSimFrame and AHSimFrame:IsShown() then
        AHSimFrame:Hide()
    else
        OpenWindow()
    end
end

SLASH_AUCTIONSIM1 = "/auctionsim"
SLASH_AUCTIONSIM2 = "/ahsim"
SlashCmdList["AUCTIONSIM"] = ToggleWindow

AHSim:RegisterHandler(AHSim.OP.WHOAMI, function(status)
    if status ~= "ok" then
        return
    end
    AHSim.authorized = true
    -- build now so the first /auctionsim is instant and no config push races it
    if AHSim.BuildWindow then
        AHSim.BuildWindow()
    end
end)
