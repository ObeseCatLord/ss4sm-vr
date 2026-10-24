// Compile against the exact private Monado source/build headers, never guessed wire offsets.
#include "remote/r_interface.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <cstddef>
#include <cerrno>
static_assert(std::endian::native==std::endian::little);
static_assert(sizeof(float)==4&&sizeof(bool)==1&&sizeof(r_remote_data)==376);
static_assert(offsetof(r_remote_data,head)==8&&offsetof(r_remote_data,left)==136&&offsetof(r_remote_data,right)==256);
using Clock=std::chrono::steady_clock;
static bool transfer(int socket,void *buffer,size_t size,bool write,Clock::time_point deadline) {
    auto *p=static_cast<unsigned char *>(buffer);
    while(size) {
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(remaining<=0)return false;
        pollfd fd{socket,static_cast<short>(write?POLLOUT:POLLIN),0};
        if(poll(&fd,1,static_cast<int>(remaining))<=0) { if(errno==EINTR)continue;return false; }
        auto n=write?send(socket,p,size,MSG_NOSIGNAL):recv(socket,p,size,0);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0)return false;p+=n;size-=static_cast<size_t>(n);
    }
    return true;
}
static xrt_quat multiply(xrt_quat a,xrt_quat b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
int main(int n,char **a) {
    if(n!=8 && n!=12 && n!=13){std::fprintf(stderr,"usage: pose_driver port x y z yaw pitch roll [left_trigger right_trigger left_yaw right_yaw | --move left_x left_y right_x right_y] (metres, radians; triggers0..1; sticks-1..1)\n");return 2;}
    char *end=nullptr;long port=std::strtol(a[1],&end,10);if(end==a[1]||*end||port<1||port>65535)return 2;
    double v[6]{};for(unsigned i=0;i<6;++i){v[i]=std::strtod(a[i+2],&end);if(end==a[i+2]||*end||!std::isfinite(v[i])||std::abs(v[i])>10)return 2;}
    double controls[4]{};
    double sticks[4]{};
    if(n==12)for(unsigned i=0;i<4;++i) {
        controls[i]=std::strtod(a[i+8],&end);
        if(end==a[i+8]||*end||!std::isfinite(controls[i]) ||
            (i<2 ? controls[i]<0 || controls[i]>1 : std::abs(controls[i])>3.141592653589793))return 2;
    }
    // A separate movement mode cannot carry trigger values. All omitted buttons
    // remain neutral in the zero-initialized packet, including both triggers.
    if(n==13) {
        if(std::strcmp(a[8],"--move")!=0)return 2;
        for(unsigned i=0;i<4;++i) {
            sticks[i]=std::strtod(a[i+9],&end);
            if(end==a[i+9]||*end||!std::isfinite(sticks[i])||std::abs(sticks[i])>1)return 2;
        }
    }
    auto deadline=Clock::now()+std::chrono::seconds(2);
    int sock=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(sock<0)return 3;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<uint16_t>(port));address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    // Loopback connection only; no host/address argument can expose another endpoint.
    if(connect(sock,reinterpret_cast<sockaddr *>(&address),sizeof(address))!=0) {
        if(errno!=EINPROGRESS){close(sock);return 3;}
        pollfd ready{sock,POLLOUT,0};int error=0;socklen_t size=sizeof(error);
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(left<=0||poll(&ready,1,static_cast<int>(left))<=0||getsockopt(sock,SOL_SOCKET,SO_ERROR,&error,&size)!=0||error) {
            close(sock);return 3;
        }
    }
    r_remote_data handshake[2]{};
    for(auto &packet:handshake) {
        if(!transfer(sock,&packet,sizeof(packet),false,deadline)||std::memcmp(&packet.header,"mndrmt3\0",8)!=0){close(sock);return 4;}
    }
    r_remote_data packet{};std::memset(&packet,0,sizeof(packet));std::memcpy(&packet.header,"mndrmt3\0",8);
    const auto yaw=xrt_quat{0,static_cast<float>(std::sin(v[3]/2)),0,static_cast<float>(std::cos(v[3]/2))};
    const auto pitch=xrt_quat{static_cast<float>(std::sin(v[4]/2)),0,0,static_cast<float>(std::cos(v[4]/2))};
    const auto roll=xrt_quat{0,0,static_cast<float>(std::sin(v[5]/2)),static_cast<float>(std::cos(v[5]/2))};
    packet.head.center.orientation=multiply(multiply(yaw,pitch),roll);
    auto &q=packet.head.center.orientation;
    const double length=std::sqrt(double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w);
    q.x/=length;q.y/=length;q.z/=length;q.w/=length;
    packet.head.center.position={static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])};
    packet.head.per_view_data_valid=true;
    for(unsigned h=0;h<2;++h) {
        auto &view=packet.head.views[h];view.pose.orientation.w=1;
        view.pose.position.x=h?.032f:-.032f;
        view.fov={-.85f,.85f,.85f,-.85f};
        auto &controller=h?packet.right:packet.left;
        controller.active=true;controller.hand_tracking_active=false;
        controller.pose.orientation={0,static_cast<float>(std::sin(controls[h+2]/2)),0,
            static_cast<float>(std::cos(controls[h+2]/2))};
        controller.pose.position={h?.25f:-.25f,1.3f,-.5f};
        controller.trigger_value.x=static_cast<float>(controls[h]);
        controller.trigger_click=controls[h]>.65;
        controller.trigger_touch=controls[h]>0;
        controller.thumbstick={static_cast<float>(sticks[2*h]),static_cast<float>(sticks[2*h+1])};
        controller.thumbstick_touch=sticks[2*h]!=0 || sticks[2*h+1]!=0;
    }
    bool ok=transfer(sock,&packet,sizeof(packet),true,deadline);close(sock);
    if(!ok)return 5;
    std::puts("Held exact synthetic pose sent; OpenXR observations must certify application");return 0;
}
