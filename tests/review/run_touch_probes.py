"""Exercise actual CST816S release fallback callbacks against a fake bus/IRQ."""
import argparse
from pathlib import Path
import subprocess
import tempfile
def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
args = parser.parse_args()
source = (Path(__file__).resolve().parents[2] / 'main/boards/CyberVoc-Board-V2_0/touch_cst816s.cc').read_text(encoding='utf-8')
prefix = r'''
#include <cstdio>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK=0, LV_INDEV_STATE_RELEASED=0, LV_INDEV_STATE_PRESSED=1;
struct lv_point_t { int x=0, y=0; };
struct lv_indev_data_t { lv_point_t point; int state=0; bool continue_reading=false; };
struct lv_indev_t {};
using lv_indev_read_cb_t = void (*)(lv_indev_t*,lv_indev_data_t*);
struct Touch;
using esp_lcd_touch_handle_t = Touch*;
struct Touch { esp_err_t (*read_data)(Touch*); struct {int x_max=360,y_max=360;} config; };
struct esp_lcd_touch_point_data_t {int x=0,y=0;};
bool irq=false,cancelled=false;
int fingers=0,x=180,y=50,read_error=0,reads=0;
lv_indev_data_t cached;
lv_indev_read_cb_t read_cb;
void ui_bridge_cancel_pointer() {cancelled=true;}
esp_err_t BusRead(Touch*) {++reads; return read_error;}
esp_err_t esp_lcd_touch_get_data(Touch*,esp_lcd_touch_point_data_t* p,uint8_t* count,int) {*count=fingers;p->x=x;p->y=y;return ESP_OK;}
Touch touch{BusRead};
void AdapterRead(lv_indev_t*,lv_indev_data_t* d) {
    if(irq) {irq=false;touch.read_data(&touch);cached.state=fingers?1:0;cached.point={x,y};}
    *d=cached;
}
lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t*) {return AdapterRead;}
void lv_indev_set_read_cb(lv_indev_t*,lv_indev_read_cb_t cb) {read_cb=cb;}
class Cst816sTouch {
public:
    Touch* handle_=&touch;
    inline static Cst816sTouch* active_=nullptr;
    esp_err_t (*driver_read_)(Touch*)=nullptr;
    lv_indev_read_cb_t adapter_read_=nullptr;
    bool fresh_=false;
    esp_err_t read_result_=0;
    lv_indev_data_t last_{};
    void attach_release_fallback(lv_indev_t*);
    static esp_err_t tracked_read(Touch*);
    static void read_input(lv_indev_t*,lv_indev_data_t*);
};
'''
callbacks = "\n".join(function(source, name) for name in ("void Cst816sTouch::attach_release_fallback(", "esp_err_t Cst816sTouch::tracked_read(", "void Cst816sTouch::read_input("))
main = r'''
int failures=0;
void Check(const char* name,bool ok){std::printf("%s: %s\n",ok?"PASS":"FAIL",name);failures+=!ok;}
int main(){
    Cst816sTouch wrapper;lv_indev_t input;lv_indev_data_t data;wrapper.attach_release_fallback(&input);
    irq=true;fingers=1;read_cb(&input,&data);Check("IRQ press reaches LVGL",data.state==1);
    fingers=0;read_cb(&input,&data);Check("release without IRQ clears cached press",data.state==0);
    int previous=reads;read_cb(&input,&data);Check("released sleeping sensor is not polled",data.state==0&&reads==previous);
    irq=true;fingers=1;read_cb(&input,&data);Check("next IRQ starts a new press",data.state==1);
    read_error=-1;read_cb(&input,&data);Check("bus failure releases and cancels click",data.state==0&&cancelled);
    read_error=0;cancelled=false;irq=true;x=400;read_cb(&input,&data);Check("invalid coordinate cancels click",data.state==0&&cancelled);
    return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='miaoban-touch-test-') as tmp:
    cpp,exe=Path(tmp)/'touch.cc',Path(tmp)/'touch.exe'
    cpp.write_text(prefix+callbacks+main,encoding='utf-8')
    subprocess.run([args.compiler,'c++','-std=c++17',str(cpp),'-o',str(exe)],check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
