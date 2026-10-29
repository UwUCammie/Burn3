// The placement table for Burnout 3's UI on a widescreen picture: the items
// that do not simply move with the centred 4:3 block. Syntax: ui_layout.hpp.
// Texture keys and areas come from the layout log (TAKEDOWN_DEV_UI_LAYOUT_LOG);
// a key is a hash of the texture's image data, the same on every screen.
R"RULES(
# Menu frame: every menu screen draws the blue arc (42884cff5f0c9ce6) in its
# top-left corner. The header band and its rules run from the arc to the right
# edge, with the screen title on the right; the arc and the section name (RACE,
# SINGLE EVENT...) stay on the left.
left    name=menu-arc     tex=42884cff5f0c9ce6
span    name=menu-header  tex=none area=70,20,640,70 when=42884cff5f0c9ce6
left    name=menu-section area=0,0,300,90 when=42884cff5f0c9ce6
right   name=menu-title   area=330,0,640,90 when=42884cff5f0c9ce6

# The footer rule runs from the left edge to its corner piece on the right
# edge. (The rule above it, at y 357, stays in the block: the menu video hides
# it, and spanning it would show it beside the video.)
span    name=menu-footer  tex=none area=0,380,610,392 when=42884cff5f0c9ce6
right   name=menu-corner  tex=6ea782fa602ce4d6 texels=0,0,32,32 area=600,355,640,392 when=42884cff5f0c9ce6

# The video behind the main menus (64-pixel tiles, new texture data every
# frame) stays in the 4:3 block: dialogs darken the tiles under them, so a
# stretched video would pull that shading out of the dialog's frame.

# The lines either side of the highlighted menu item reach the screen edges.
pin     name=menu-marker  tex=5cc8b38661dc624b

# Race HUD (while the boost bar's frame, 3f7903c13f585625, is drawn; the panels
# 2773e574a70bd410 also frame the results and Burnout Points screens): each corner stays
# in its corner with everything in it (POS, LAP, boost bar and its stars and
# flames, speedometer).
left    name=hud-top-left     area=0,0,200,110 when=3f7903c13f585625
right   name=hud-top-right    area=440,0,640,110 when=3f7903c13f585625
# The full boost bar (Time Attack) reaches x 390; the speedometer starts at 442.
# By where a piece starts: the flames of a full bar stream far past its end
# and must still move with it. (Wider pieces, full-screen flashes, stay out.)
# Bottom corners, by texture where the image is a fixed one (the boost bar's
# frame, its stars and icons, the speedometer's panel). The bar's animated
# fill and flames get a new texture hash every frame, so the region rules
# below catch those and the text.
left    name=hud-boost-frame  tex=3f7903c13f585625 when=3f7903c13f585625
left    name=hud-boost-icon   tex=a0e374be6fe7141 when=3f7903c13f585625
left    name=hud-boost-star   tex=3c2a3e7fac141fff when=3f7903c13f585625
left    name=hud-boost-piece  tex=31d4ac02e63c8c55 when=3f7903c13f585625
left    name=hud-boost-label  tex=6e7bfbd4c7f6bb34 when=3f7903c13f585625
left    name=hud-boost-fx-a   tex=69f97eb289c6d8e4 when=3f7903c13f585625
left    name=hud-boost-fx-b   tex=7ae9028a4cd64ece when=3f7903c13f585625
left    name=hud-boost-fx-c   tex=2d5c10e91b7fd448 when=3f7903c13f585625
right   name=hud-speedo-panel tex=2773e574a70bd410 area=420,340,640,448 when=3f7903c13f585625
left    name=hud-bottom-left  origin=0,300,420,448 max_size=480,148 when=3f7903c13f585625
right   name=hud-bottom-right origin=420,300,640,448 max_size=220,148 when=3f7903c13f585625
)RULES"
