#pragma once
#include <switch.h>
#include "so_util.h"

enum { ROUTE_UI = 0, ROUTE_TUTORIAL = 1, ROUTE_GAME = 2 };

void input_route_init(so_module *mod);
void input_route_update(void); /* once per frame, before input is handed out */
int input_route(void);
void *input_route_tutorial_menu(void); /* tutorial step confirmed with A, or NULL */
int input_route_android(u64 key); /* press goes to ButtonManager */
int input_route_hw(u64 key);      /* press goes to JoystickHW */
int input_route_cursor_tap(void); /* Cursor may turn an A press into a tap */
