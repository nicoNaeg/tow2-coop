-- Print the class chain, functions and object properties of the first live widget of class WIDGET.
local WIDGET = WIDGET or "DialogBox_Photosensitivity_BP_C"
local target
for _, w in ipairs(FindAllOf("UserWidget") or {}) do
    if w:IsValid() and w:GetClass():GetFName():ToString() == WIDGET and not w:GetFullName():find("Default__") then
        target = w
        break
    end
end
if not target then
    print("no live " .. WIDGET)
    return
end
print("object: " .. target:GetFullName())
local cls = target:GetClass()
while cls and cls:IsValid() do
    local name = cls:GetFName():ToString()
    local fns = {}
    cls:ForEachFunction(function(f)
        fns[#fns + 1] = f:GetFName():ToString()
    end)
    local props = {}
    cls:ForEachProperty(function(p)
        props[#props + 1] = p:GetFName():ToString() .. ":" .. p:GetClass():GetFName():ToString()
    end)
    print(string.format("== %s  (%d functions, %d properties)", name, #fns, #props))
    if #fns > 0 then print("  fn: " .. table.concat(fns, ", ")) end
    if #props > 0 then print("  prop: " .. table.concat(props, ", ")) end
    if name == "UserWidget" then break end
    cls = cls:GetSuperStruct()
end
