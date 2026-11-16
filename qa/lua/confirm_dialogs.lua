-- Confirm every live DialogBoxWidget (first-run warnings, notices) through its own native ExecuteConfirm.
local dialogBase = StaticFindObject("/Script/Arkansas.DialogBoxWidget")
local n = 0
for _, w in ipairs(FindAllOf("UserWidget") or {}) do
    if w:IsValid() and not w:GetFullName():find("Default__") and dialogBase:IsValid() and w:IsA(dialogBase) then
        print("confirm " .. w:GetClass():GetFName():ToString())
        w:ExecuteConfirm()
        n = n + 1
    end
end
print("confirmed " .. n)
