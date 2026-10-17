// SPDX-License-Identifier: CC0-1.0
// Battle HUD (BattleHUD_DLC.gfx): the action map with a controller, laid out as on PC.
//
// The action map (`buttons`: X / Y / B with their labels and the ability meter) is a column of
// big touch buttons in the top left corner on Android. With a controller the PC version shows it
// around the active fighter instead: small (32 px) button icons in a diamond, X's label to the
// left, the ability (Y) above, B's to the right, at the point the engine pushes as
// ButtonMapScreenXCenter / YCenter (host/src/gamepad.cpp computes it as the PC does). These are
// the PC movie's own values (its "Console" frame). In touch mode (ShowPCButtons true: on Android
// that means the touch pictures) Android's layout is put back, with the column's backdrop
// (cq64_panel, named by datapatch.cpp). updateButtons(), which the HUD
// calls every frame, is wrapped; see docs/battle-controller.md.
// Added after the movie's frame-1 script by datapatch.cpp.
var cq64_padLayout = [
   ["button_x", -48, -91, 100, 100],
   ["button_y", -16, -123, 100, 100],
   ["button_b", 16, -91, 100, 100],
   ["abilityMeter", 0, -107, 16.453552, 16.453552],
   // text fields: x, y, width, alignment
   ["xText", -411, -94, 360, "right"],
   ["yText", -248, -162, 500, "center"],
   ["bText", 54, -94, 360, "left"]
];
var cq64_updateButtons = updateButtons;
updateButtons = function()
{
   cq64_updateButtons();
   cq64_layoutButtons();
   cq64_runAway();
};
// "LT + RT  RUN AWAY": the PC HUD's runAway clip at (1030, 80), per language: the white box's
// offset and width scale (its shape: -40..200 x -31..29), the text's x and width (right-aligned,
// $GoodDog 30), the icons' corner. The icons (cq64_LT / cq64_RT) are added by datapatch.cpp.
var cq64_runAwayLayout = {kLANG_enUS:["RUN AWAY", -33.35, 1.1666718, -37, 225, -73, -27],
   kLANG_esES:["HUIR", 29.15, 0.85417175, 38, 150, 3, -27],
   kLANG_deDE:["WEGRENNEN", -50, 1.25, -57, 245, -93, -28.15],
   kLANG_frFR:["FUIR", 29.15, 0.85417175, 38, 150, 2, -27.15],
   kLANG_itIT:["SCAPPA", 0, 1, 3, 185, -34, -27.15]};
function cq64_text(mc, name, x, y, w, h, text, align)
{
   mc.createTextField(name, mc.getNextHighestDepth(), x, y, w, h);
   var t = mc[name];
   var tf = new TextFormat();
   tf.font = "$GoodDog";
   tf.size = 30;
   tf.color = 0;
   tf.align = align;
   t.embedFonts = true;
   t.selectable = false;
   t.setNewTextFormat(tf);
   t.text = text;
   return t;
}
function cq64_makeRunAway()
{
   var r = _root.createEmptyMovieClip("cq64_runAwayBox", 186);
   var l = cq64_runAwayLayout[GetCurrentLanguage()];
   if(l == undefined)
   {
      l = cq64_runAwayLayout.kLANG_enUS;
   }
   r._x = 1030;
   r._y = 80;
   r.beginFill(0xFFFFFF, 100);
   r.moveTo(l[1] - 40 * l[2], -31);
   r.lineTo(l[1] + 200 * l[2], -31);
   r.lineTo(l[1] + 200 * l[2], 29);
   r.lineTo(l[1] - 40 * l[2], 29);
   r.endFill();
   cq64_text(r, "runText", l[3] - 2, -16, l[4] + 2, 44, l[0], "right");
   var b = r.createEmptyMovieClip("runawayButtons", r.getNextHighestDepth());
   b._x = l[5];
   b._y = l[6];
   var i = b.attachMovie("cq64_LT", "lt", 1);
   i._xscale = i._yscale = 168.75;
   i = b.attachMovie("cq64_RT", "rt", 2);
   i._x = 66;
   i._xscale = i._yscale = 168.75;
   cq64_text(b, "plus", 40, 11, 40, 44, "+", "center");
   return r;
}
function cq64_runAway()
{
   var pad = _global.ShowPCButtons == false;
   if(cq64_runAwayBox == undefined)
   {
      cq64_makeRunAway();
   }
   cq64_runAwayBox._visible = pad && _global.BattleCanRunAway == true;  // as the PC shows runAway
   // with a controller: Start pauses and LT + RT runs away, as on PC
   runaway_button._visible = !pad && _global.BattleCanRunAway;
   pause_button._visible = !pad && _global.BattleShowPause;
}
function cq64_align(field, align)
{
   var tf = new TextFormat();
   tf.align = align;
   field.setTextFormat(tf);
   field.setNewTextFormat(tf);
   field.cq64_text = field.text;
}
function cq64_layoutButtons()
{
   var b = buttons;
   var pad = _global.ShowPCButtons == false;
   var i;
   var e;
   var c;
   if(b.cq64_touch == undefined)
   {
      // Android's own layout, to go back to in touch mode
      b.cq64_touch = {x:b._x, y:b._y, kids:[]};
      i = 0;
      while(i < cq64_padLayout.length)
      {
         c = b[cq64_padLayout[i][0]];
         b.cq64_touch.kids.push({mc:c, x:c._x, y:c._y, xs:c._xscale, ys:c._yscale, w:c._width});
         i++;
      }
      b.cq64_touch.bg = b.button_b_bg._visible;
   }
   if(pad != b.cq64_pad)
   {
      b.cq64_pad = pad;
      i = 0;
      while(i < cq64_padLayout.length)
      {
         e = cq64_padLayout[i];
         c = b.cq64_touch.kids[i];
         if(typeof e[4] == "string")
         {
            c.mc._x = !pad ? c.x : e[1];
            c.mc._y = !pad ? c.y : e[2];
            c.mc._width = !pad ? c.w : e[3];
            cq64_align(c.mc, !pad ? "left" : e[4]);
         }
         else
         {
            c.mc._x = !pad ? c.x : e[1];
            c.mc._y = !pad ? c.y : e[2];
            c.mc._xscale = !pad ? c.xs : e[3];
            c.mc._yscale = !pad ? c.ys : e[4];
         }
         i++;
      }
      b.button_b_bg._visible = !pad ? b.cq64_touch.bg : false;
      // the touch column's backdrop (named by datapatch.cpp)
      b.cq64_panel._visible = !pad;
      // the prompt's button under the timing bar: PC 50 % (a 32 px icon) at (74, -2.7);
      // Android draws its 64 px icon at 100 %, 22 px lower
      c = progressBar.instruction.button;
      if(b.cq64_touch.prompt == undefined)
      {
         b.cq64_touch.prompt = {x:c._x, y:c._y, xs:c._xscale, ys:c._yscale};
      }
      c._x = !pad ? b.cq64_touch.prompt.x : 74;
      c._y = !pad ? b.cq64_touch.prompt.y : -2.7;
      c._xscale = !pad ? b.cq64_touch.prompt.xs : 50;
      c._yscale = !pad ? b.cq64_touch.prompt.ys : 50;
      if(!pad)
      {
         b._x = b.cq64_touch.x;
         b._y = b.cq64_touch.y;
      }
   }
   if(pad)
   {
      if(_global.ButtonMapScreenXCenter > -1000)
      {
         b._x = _global.ButtonMapScreenXCenter;
         b._y = _global.ButtonMapScreenYCenter;
      }
      // labels change as the menu moves on: keep their alignment
      if(b.xText.text != b.xText.cq64_text)
      {
         cq64_align(b.xText, "right");
      }
      if(b.yText.text != b.yText.cq64_text)
      {
         cq64_align(b.yText, "center");
      }
      if(b.bText.text != b.bText.cq64_text)
      {
         cq64_align(b.bText, "left");
      }
   }
}
