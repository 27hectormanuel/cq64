// SPDX-License-Identifier: CC0-1.0
// Stamp shop (StampShop_DLC.gfx): choosing stamps with a controller.
//
// The shop shows 5 of its stamps at a time. On PC, ShopMenu.layoutControls() scrolls the list to
// the selected stamp. The Android release turned that around for its touch arrows: it pulls the
// selection back into the visible rows instead. With a controller (Up / Down, prevItem() /
// nextItem() -> setCurrentItem()), Down on the last visible stamp was put back on it, so the
// stamps below could never be selected. Here setCurrentItem() first scrolls the list to the
// stamp, as on PC; the touch arrows (which move the list, not the selection) work as before.
// Added after the movie's frame-1 script by datapatch.cpp.
StampShop.ShopMenu.prototype.setCurrentItem = function(item)
{
   if(item < this.itemControls.length)
   {
      if(item < this.firstVisibleControl)
      {
         this.firstVisibleControl = item;
      }
      else if(item >= this.firstVisibleControl + this.maxVisibleControls)
      {
         this.firstVisibleControl = item - (this.maxVisibleControls - 1);
      }
      this.currentItem = item;
      _global.g_Navigator.SetCurrentFocus(this.itemControls[this.currentItem]);
      this.layoutControls();
   }
};
