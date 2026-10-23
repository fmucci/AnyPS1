/* Coverage and serial hooks are defined in sys.c / hle.c of the whole-game build; builds of io.c alone (the oracle's shared library) get inert versions. */
#include <stdint.h>
int rt_cov_on;
void rt_cov(int group, unsigned id) { (void)group; (void)id; }
