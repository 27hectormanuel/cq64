// SPDX-License-Identifier: CC0-1.0
// Journal (Notebook_DLC.gfx), Quests page: scrolling with a controller and the mouse wheel.
//
// The Android release added touch dragging to the two quest lists (Notebook.QuestHolder). Every
// frame, Notebook.Quests.update() sets each list's _y from its scrollbar (scrollBars()), then
// overwrites it with QuestHolder.getTargetY(), the position of the last finger drag, and moves
// the scrollbar back to match. The stick (handleInput(), left stick = active quests, right stick =
// completed ones) and the mouse wheel only move the scrollbar, so the next frame undid them.
// Here the drag target follows the list whenever no drag is in progress, so all of them work.
// Added after the movie's frame-1 script by datapatch.cpp.
Notebook.QuestHolder.prototype.getTargetY = function()
{
   if(!this.dragging)
   {
      this.target_y = this._y;
   }
   return this.target_y;
};
