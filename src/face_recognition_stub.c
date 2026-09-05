#include "face_recognition.h"

int zh_face_recognition_start(void) {
    return 0;
}

void zh_face_recognition_stop(void) {
}

void zh_face_recognition_set_active(int active) {
    (void)active;
}

void zh_face_recognition_set_ws(zh_ws_session_t *ws) {
    (void)ws;
}

int zh_face_enroll_on_recog(void) {
    return -1;
}

int zh_face_enroll_on_owner(const char *name) {
    (void)name;
    return -1;
}
