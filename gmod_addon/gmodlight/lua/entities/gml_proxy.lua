-- Invisible physics stand-in for a Dying Light actor. The physgun grabs this;
-- GModLight moves the real zombie in Dying Light to match, and damage done to it
-- (see EntityTakeDamage in autorun/gmodlight.lua) is done to the real zombie.
-- Its feet are at the entity origin and its local Z runs up the body; the length
-- follows the actor's pose (standing, crouched, crawling, lying), see SetShape.
AddCSLuaFile()

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.PrintName = "GModLight actor"
ENT.Spawnable = false

local HALF = 12           -- half width, units (~0.46 m wide)
local DEFAULT_LEN = 94    -- about 1.8 m

local function bounds(len)
	return Vector(-HALF, -HALF, 0), Vector(HALF, HALF, len)
end

function ENT:SetupDataTables()
	self:NetworkVar("Float", 0, "BodyLength")
end

function ENT:Initialize()
	self:SetModel("models/hunter/blocks/cube025x025x025.mdl")
	self:DrawShadow(false)
	local len = self:GetBodyLength() > 0 and self:GetBodyLength() or DEFAULT_LEN
	-- The model is a small cube at the feet; without this the box isn't drawn
	-- (or hit by client traces) whenever the feet are off screen.
	if CLIENT then self:SetRenderBounds(bounds(len)) end
	if SERVER then
		self:SetBodyLength(DEFAULT_LEN)
		self:InitBox(DEFAULT_LEN, false)
		self:SetMoveType(MOVETYPE_VPHYSICS)
		self:SetSolid(SOLID_VPHYSICS)
		self:EnableCustomCollisions(true)
	end
end

if SERVER then
	function ENT:InitBox(len, motion)
		local mins, maxs = bounds(len)
		self:PhysicsInitBox(mins, maxs)
		self:SetCollisionBounds(mins, maxs)
		local phys = self:GetPhysicsObject()
		if IsValid(phys) then
			phys:SetMass(70)
			phys:EnableMotion(motion)
			phys:Wake()
		end
	end

	-- Body length in units. Rebuilt only on a real change: it re-creates the physics object.
	function ENT:SetShape(len)
		len = math.Clamp(len, 20, 115)
		if math.abs(len - self:GetBodyLength()) < 8 then return end
		local phys = self:GetPhysicsObject()
		self:SetBodyLength(len)
		self:InitBox(len, IsValid(phys) and phys:IsMotionEnabled() or false)
	end
end

function ENT:Think()
	if CLIENT then
		local len = self:GetBodyLength()
		if len > 0 and len ~= self.GMLDrawnLen then
			self.GMLDrawnLen = len
			self:SetRenderBounds(bounds(len))
		end
	end
end

function ENT:Draw()
	-- Dying Light draws the real actor. Show the box only while debugging (F9).
	if GML and GML.debugBoxes then
		local mins, maxs = bounds(self:GetBodyLength() > 0 and self:GetBodyLength() or DEFAULT_LEN)
		render.DrawWireframeBox(self:GetPos(), self:GetAngles(), mins, maxs, self:GetNWBool("GMLDriven") and Color(255, 160, 0) or Color(0, 255, 0), true)
	end
end

if SERVER then
	-- Props flung into the zombie hurt it, and so does the zombie slamming into
	-- things after being thrown. Physics impacts don't go through EntityTakeDamage
	-- for this kind of entity, so they're turned into damage here.
	function ENT:PhysicsCollide(data)
		self.GMLImpact = math.max(self.GMLImpact or 0, data.Speed)
		if CurTime() - (self.GMLLastHit or 0) < 0.3 then return end
		-- How sharply each body was stopped, in m/s: a real hit, not sliding along.
		local ours = (data.OurOldVelocity - data.OurNewVelocity):Length() / 52.49
		local theirs = (data.TheirOldVelocity - data.TheirNewVelocity):Length() / 52.49
		local other = data.HitObject
		local dmg = 0
		if IsValid(other) and other:IsMoveable() and theirs > 4 then
			dmg = other:GetMass() * theirs * 0.05      -- hit by something thrown
		elseif self.GMLDriven and not self.GMLHeld and ours > 6 then
			dmg = 70 * ours * 0.05                      -- thrown into something
		end
		if dmg < 1 then return end
		self.GMLLastHit = CurTime()
		GML.DamageActor(self, math.min(dmg, 400), data.HitPos, -data.HitNormal * data.Speed, DMG_CRUSH)
	end
end
