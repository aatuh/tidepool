/* Compile the application as part of the test TU to examine the real model. */
#define main tidepool_application_main
#include "../tidepool.c"
#undef main
#include <assert.h>

static void verify_grid(Garden *g) {
    size_t n = (size_t)g->w*g->h;
    float lo=1, hi=0, sum=0;
    for (size_t i=0; i<n; ++i) {
        assert(isfinite(g->u[i]) && isfinite(g->v[i]));
        assert(g->u[i]>=0 && g->u[i]<=1);
        assert(g->v[i]>=0 && g->v[i]<=1);
        if (g->v[i]<lo) lo=g->v[i];
        if (g->v[i]>hi) hi=g->v[i];
        sum+=g->v[i];
    }
    assert(hi > .15f && hi < .8f);
    assert(lo < .08f);
    assert(sum/n > .005f);
}

/* Evaluate differences after the ordinary chemical dynamics have settled.
   Verifies renewal is sustained and does not simply consume the whole garden. */
static float mature_change(RenewalMode renewal) {
    App a = {0};
    a.config.hud = 1;
    a.config.layout = LAYOUT_US;
    a.config.utf8 = 1;
    a.config.color = COLOR_TRUE;
    a.config.renewal = renewal;
    a.rng = 743927;
    make_keys(&a);
    assert(garden_resize(&a,100,34));
    assert(a.grazer_count == (renewal == RENEWAL_OFF ? 0 :
        renewal == RENEWAL_GENTLE ? 2 : 4));
    /* Let a typical pattern settle, then measure a later 500-step window. */
    for (int i = 0; i < 8500; ++i) {
        garden_step(&a.garden);
        garden_ecology_step(&a);
    }
    size_t n = (size_t)a.garden.w*a.garden.h;
    float *before = malloc(n*sizeof(float));
    assert(before);
    memcpy(before,a.garden.v,n*sizeof(float));
    for (int i = 0; i < 500; ++i) {
        garden_step(&a.garden);
        garden_ecology_step(&a);
    }
    verify_grid(&a.garden);
    double change = 0.0;
    for (size_t i = 0; i < n; ++i)
        change += fabsf(a.garden.v[i]-before[i]);
    float average = (float)(change/n);
    free(before);

    if (renewal != RENEWAL_OFF) {
        assert(a.garden_cover > .080f); /* Not depleted to sterile U. */
        assert(garden_resize(&a,140,42));
        for (int k = 0; k < a.grazer_count; ++k) {
            assert(a.grazers[k].x >= 0.f && a.grazers[k].x < a.garden.w);
            assert(a.grazers[k].y >= 0.f && a.grazers[k].y < a.garden.h);
        }
        for (int i = 0; i < 100; ++i) {
            garden_step(&a.garden);
            garden_ecology_step(&a);
        }
        verify_grid(&a.garden);
    }
    garden_dispose(&a.garden);
    return average;
}

static void verify_options(void) {
    Config c={0};
    char *off[] = {(char *)"tidepool",(char *)"--renewal",(char *)"off"};
    char *gentle[] = {(char *)"tidepool",(char *)"--renewal",(char *)"gentle"};
    char *active[] = {(char *)"tidepool",(char *)"--renewal",(char *)"active"};
    assert(parse_args(1,off,&c)==1 && c.renewal==RENEWAL_OFF);
    assert(parse_args(3,off,&c)==1 && c.renewal==RENEWAL_OFF);
    assert(parse_args(3,gentle,&c)==1 && c.renewal==RENEWAL_GENTLE);
    assert(parse_args(3,active,&c)==1 && c.renewal==RENEWAL_ACTIVE);
}

static void verify_exit_input(void) {
    for (int state = 0; state <= 3; ++state) {
        for (int byte = 3; byte <= 4; ++byte) {
            App a = {0};
            a.input_state = state;
            a.utf_expected = 2;
            stop_requested = 0;
            app_input_byte(&a, (unsigned char)byte);
            assert(stop_requested);
        }
    }
    stop_requested = 0;
}

static void verify_nonblocking_render(void) {
    App a = {0};
    a.config.hud = 1;
    a.config.utf8 = 1;
    a.config.color = COLOR_TRUE;
    a.rng = 743927;
    make_keys(&a);
    assert(garden_resize(&a,149,43));
    int original_stdout = dup(STDOUT_FILENO);
    int sink = open("/dev/null", O_WRONLY);
    assert(original_stdout >= 0 && sink >= 0);
    assert(dup2(sink,STDOUT_FILENO) >= 0);
    double started = seconds_now();
    app_render(&a);
    double elapsed = seconds_now()-started;
    assert(dup2(original_stdout,STDOUT_FILENO) >= 0);
    close(original_stdout); close(sink);
    assert(a.output.len > 0 && a.output.len <= a.output.capacity && !a.output.failed);
    int brush_cell = 40*a.garden.cols+145;
    assert(a.garden.last[brush_cell].glyph != UINT32_MAX); /* One coherent snapshot. */
    uint32_t before_brush = a.garden.last[brush_cell].fg;
    a.output.len = 0; /* The previous frame has drained. */
    a.next_output_at = 0;
    garden_touch(&a.garden,145.f,80.f,.92f,0);
    app_render(&a);
    assert(a.garden.last[brush_cell].glyph != UINT32_MAX);
    assert(a.garden.last[brush_cell].fg != before_brush);
    assert(!a.output.failed && a.output.len <= a.output.capacity);
    garden_dispose(&a.garden);
    assert(elapsed < .05); /* Drawing must return control within one tick. */
}

static void verify_dense_paint_output(void) {
    App a = {0};
    a.config.hud = 1;
    a.config.utf8 = 1;
    a.config.color = COLOR_TRUE;
    a.indexed_colors = 1;
    a.rng = 743927;
    make_keys(&a);
    assert(garden_resize(&a,149,43));
    app_render(&a);
    for (int y = 3; y < 82; y += 8)
        for (int x = 3; x < 146; x += 8)
            garden_touch(&a.garden,(float)x,(float)y,.92f,0);
    size_t bytes = 0;
    for (int frame = 0; frame < 120; ++frame) {
        garden_step(&a.garden); garden_step(&a.garden);
        garden_trace_fade(&a.garden);
        a.output.len = 0;
        a.next_output_at = 0;
        app_render(&a);
        assert(!a.output.failed);
        bytes += a.output.len;
        installed_palette = a.output_palette;
    }
    /* A heavily painted screen must average at least 15 complete snapshots
       per second within the terminal's output budget. */
    assert(bytes/120 < OUTPUT_BYTES_PER_SECOND/15);
    garden_dispose(&a.garden);
    installed_palette = -1;
}

/* Interpret cursor movement and glyphs, so shorter cursor commands must still
   draw a complete frame at the correct terminal positions. */
static void apply_frame_glyphs(const App *a, uint32_t *screen) {
    const Output *o = &a->output;
    int x = 0, y = 0;
    for (size_t i = 0; i < o->len;) {
        if ((unsigned char)o->bytes[i] == 27) {
            assert(i+1 < o->len);
            if (o->bytes[i+1] == ']') {
                i += 2;
                while (i+1 < o->len && !(o->bytes[i] == 27 && o->bytes[i+1] == '\\')) ++i;
                assert(i+1 < o->len);
                i += 2;
            } else {
                assert(o->bytes[i+1] == '[');
                i += 2;
                int params[8] = {0}, count = 0;
                while (i < o->len && (isdigit((unsigned char)o->bytes[i]) || o->bytes[i] == ';')) {
                    if (o->bytes[i] == ';') { ++count; assert(count < 8); }
                    else params[count] = params[count]*10+o->bytes[i]-'0';
                    ++i;
                }
                assert(i < o->len);
                char command = o->bytes[i++];
                if (command == 'H') { y = params[0]-1; x = params[1]-1; }
                else if (command == 'C') x += params[0] ? params[0] : 1;
                else assert(command == 'm' || command == 'J' || command == 'K');
            }
        } else if ((unsigned char)o->bytes[i] < 32) ++i;
        else {
            int consumed;
            uint32_t cp = decode_one_utf8((const unsigned char *)o->bytes+i, &consumed);
            assert(i+(size_t)consumed <= o->len);
            i += (size_t)consumed;
            if (y >= 0 && y < a->garden.picture_rows) {
                assert(x >= 0 && x < a->garden.cols);
                screen[y*a->garden.cols+x] = cp;
            }
            ++x;
        }
    }
    for (int i = 0; i < a->garden.cols*a->garden.picture_rows; ++i)
        assert(screen[i] == a->garden.last[i].glyph);
}

static void verify_mature_renewal_paint_output(RenewalMode renewal) {
    App a = {0};
    a.config.hud = 1;
    a.config.utf8 = 1;
    a.config.color = COLOR_TRUE;
    a.config.renewal = renewal;
    a.indexed_colors = 1;
    a.rng = 743927;
    make_keys(&a);
    assert(garden_resize(&a,155,61));
    for (int step = 0; step < 9000; ++step) {
        garden_step(&a.garden);
        garden_ecology_step(&a);
    }
    uint32_t *screen = calloc((size_t)a.garden.cols*a.garden.picture_rows,sizeof(*screen));
    assert(screen);
    app_render(&a);
    apply_frame_glyphs(&a,screen);
    installed_palette = a.output_palette;
    size_t bytes = 0;
    for (int frame = 0; frame < 120; ++frame) {
        garden_step(&a.garden); garden_ecology_step(&a);
        garden_step(&a.garden); garden_ecology_step(&a);
        garden_trace_fade(&a.garden);
        for (int paint = 0; paint < 3; ++paint)
            garden_touch(&a.garden,(float)(10+(frame*5+paint*8)%(a.garden.w-20)),
                (float)(10+frame%(a.garden.h-20)),.35f,0);
        a.output.len = 0;
        a.next_output_at = 0;
        app_render(&a);
        assert(!a.output.failed);
        apply_frame_glyphs(&a,screen);
        bytes += a.output.len;
    }
    printf("mature renewal %d with painting: %zu bytes/frame\n",renewal,bytes/120);
    assert(bytes/120 < app_output_rate(&a)/30);
    free(screen);
    garden_dispose(&a.garden);
    installed_palette = -1;
}

int main(void) {
    App a={0};
    a.config.hud=1;
    a.config.layout=LAYOUT_FI;
    a.config.utf8=1;
    a.config.color=COLOR_TRUE;
    a.rng=743927;
    make_keys(&a);
    assert(a.key_count>35);
    assert(garden_resize(&a,100,34));
    assert(a.garden.w==100 && a.garden.h==66);
    verify_grid(&a.garden);

    int found_ae=0, found_oe=0, found_aa=0;
    for(int k=0;k<a.key_count;++k) {
        if(a.keys[k].cp==0xe4) found_ae=1;
        if(a.keys[k].cp==0xf6) found_oe=1;
        if(a.keys[k].cp==0xe5) found_aa=1;
    }
    assert(found_ae&&found_oe&&found_aa);

    float *oldv = malloc((size_t)a.garden.w*a.garden.h*sizeof(float));
    assert(oldv);
    memcpy(oldv,a.garden.v,(size_t)a.garden.w*a.garden.h*sizeof(float));
    garden_key_touch(&a,'q');
    assert(a.last_key=='q');
    double left_delta=0,right_delta=0;
    for(int y=0;y<a.garden.h;++y) for(int x=0;x<a.garden.w;++x) {
        int i=y*a.garden.w+x;
        float d=fabsf(a.garden.v[i]-oldv[i]);
        if(x<a.garden.w/2) left_delta+=d;
        else right_delta+=d;
    }
    assert(left_delta>right_delta*20);
    memcpy(oldv,a.garden.v,(size_t)a.garden.w*a.garden.h*sizeof(float));
    garden_key_touch(&a,'p');
    left_delta=right_delta=0;
    for(int y=0;y<a.garden.h;++y) for(int x=0;x<a.garden.w;++x) {
        int i=y*a.garden.w+x;
        float d=fabsf(a.garden.v[i]-oldv[i]);
        if(x<a.garden.w/2) left_delta+=d;
        else right_delta+=d;
    }
    assert(right_delta>left_delta*20);
    garden_key_touch(&a,0xc4); /* capital Ä */
    assert(a.last_key==0xe4);
    garden_key_touch(&a,'+'); /* real Finnish key, not shifted US = */
    assert(a.last_key=='+');
    a.speed=1;
    app_arrow(&a,'C'); assert(a.speed==2);
    app_arrow(&a,'D'); assert(a.speed==1);
    app_arrow(&a,'A'); assert(a.config.palette==1);
    app_arrow(&a,'B'); assert(a.config.palette==0);

    for(int step=0;step<300;++step) garden_step(&a.garden);
    verify_grid(&a.garden);
    assert(garden_resize(&a,120,40));
    verify_grid(&a.garden);
    garden_make_overlay(&a);
    int labels=0;
    for(int i=0;i<a.garden.cols*a.garden.picture_rows;++i) labels+=a.garden.key_overlay[i]!=0;
    assert(labels>30);
    RGB c=palette_sample(0,.24f,0);
    assert(c.r>10 && c.g>22 && c.b>32);
    assert(encode_color(c,COLOR_256)>=16 && encode_color(c,COLOR_256)<=255);
    assert(encode_color(c,COLOR_16)<=15);
    free(oldv);
    garden_dispose(&a.garden);
    verify_options();
    verify_exit_input();
    verify_nonblocking_render();
    verify_dense_paint_output();
    verify_mature_renewal_paint_output(RENEWAL_GENTLE);
    verify_mature_renewal_paint_output(RENEWAL_ACTIVE);
    float off_change = mature_change(RENEWAL_OFF);
    float gentle_change = mature_change(RENEWAL_GENTLE);
    float active_change = mature_change(RENEWAL_ACTIVE);
    assert(off_change < .008f);
    assert(gentle_change > .012f && gentle_change > off_change*4.f);
    assert(active_change > .03f && active_change > gentle_change*1.5f);
    printf("late-stage change per cell (500 steps): off %.4f, gentle %.4f, active %.4f\n",
        off_change, gentle_change, active_change);
    puts("unit tests: OK — Gray–Scott, geometry, input, renewable grazing, bounded long-run evolution");
    return 0;
}
