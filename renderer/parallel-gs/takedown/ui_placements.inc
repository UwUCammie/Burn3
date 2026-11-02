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

# The takedowns counter (top left: panel, label, number) stays on the left
# edge, also while a takedown cam's bars cover it and while it slides back in
# from the top edge after the cam. The number is matched by where it starts,
# so a wider (two-digit) number moves with it.
left    name=hud-takedowns-panel tex=2773e574a70bd410 area=0,0,200,60 when=3f7903c13f585625
left    name=hud-takedowns-label tex=38cf308a66a06426 texels=0,23,230,219 text=1 area=0,0,150,46 min_size=100,0 when=3f7903c13f585625
left    name=hud-takedowns-count tex=38cf308a66a06426 origin=100,0,165,40 min_size=10,28 max_size=70,45 when=3f7903c13f585625
left    name=hud-takedowns-count-in tex=38cf308a66a06426 origin=100,0,165,0 min_size=10,10 max_size=70,45 when=3f7903c13f585625
# Takedown cam (bars 4357c5866e06aa4a, the thin bar piece: the title screen
# draws its top panel with the same texture) and crash cam (skull
# 172357bc3753484b): the title (icon, name, points; its font and icons only)
# goes to the top left corner like the HUD.
# It is drawn letter by letter in the same font as the HUD, and its letters
# fly in large, so every piece is matched by where it starts and the title
# moves as one. The score panel in the middle stays in the block (its text
# is one line ending above the title's). In the crash cam IMPACT TIME (icon,
# or the R1 button, and its label) goes to the top right corner; the icon is
# drawn a little before the skull.
centre  name=crash-cam-score    tex=2773e574a70bd410 origin=200,0,440,60 when=172357bc3753484b
centre  name=takedown-cam-score tex=2773e574a70bd410 origin=200,0,440,60 when=4357c5866e06aa4a when_texels=0,0,40,4
centre  name=hud-score-text tex=38cf308a66a06426 area=260,0,372,44 text=1 when=2773e574a70bd410
right   name=crash-impact-icon   tex=5f8261e46c6e4335
right   name=crash-impact-button tex=75a6d9da114b2750 when=172357bc3753484b
right   name=crash-impact-label  tex=38cf308a66a06426 origin=430,15,640,45 max_size=210,45 when=5f8261e46c6e4335
right   name=crash-impact-label-r1 tex=38cf308a66a06426 origin=430,15,640,45 max_size=210,45 when=75a6d9da114b2750
left    name=crash-cam-title     tex=38cf308a66a06426 origin=0,0,430,100 max_size=520,200 when=172357bc3753484b
left    name=crash-cam-skull     tex=172357bc3753484b origin=0,0,430,100 when=172357bc3753484b
left    name=takedown-cam-title  tex=38cf308a66a06426 origin=0,0,430,100 max_size=520,200 when=4357c5866e06aa4a when_texels=0,0,40,4
left    name=takedown-cam-icon   texels=0,0,128,128 origin=0,0,430,40 max_size=130,100 when=4357c5866e06aa4a when_texels=0,0,40,4

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
