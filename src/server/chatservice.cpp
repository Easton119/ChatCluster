
#include "chatservice.hpp"
#include "public.hpp"
#include "user.hpp"
#include<muduo/base/Logging.h>
#include<string>
#include<vector>
using namespace std;
using namespace muduo;
//获取单例对象的接口函数
ChatService* ChatService::instance()
{
    static ChatService service;
    return &service;
}
//注册消息以及对应的Handler回调操作
ChatService::ChatService(){
    _msgHandlerMap.insert({LOGIN_MSG,std::bind(&ChatService::login,this,_1,_2,_3)});
    _msgHandlerMap.insert({REG_MSG,std::bind(&ChatService::reg,this,_1,_2,_3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG,std::bind(&ChatService::oneChat,this,_1,_2,_3)});
}
//服务器异常，业务重置方法
void ChatService::reset(){
    _userModel.resetState();
}

//获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid){
    //记录错误日志，msgid没有对应的事件处理回调
    auto it = _msgHandlerMap.find(msgid);
    if( it == _msgHandlerMap.end()){
        //返回一个默认的处理器，空操作 
        // lamda 表达式
        return [=](const TcpConnectionPtr &conn,json &js,Timestamp time){
            LOG_ERROR<<"msgid: "<<msgid<<" can not find handler!";
        };
    }
    else{
        return _msgHandlerMap[msgid];
    }
   
}

//处理登录业务
void ChatService::login(const TcpConnectionPtr &conn,json &js,Timestamp time){
    // LOG_INFO<<"do login service!!!";
    int id = js["id"];
    string pwd = js["password"];

    User user = _userModel.query(id);
    if(user.getId()==id&&user.getPwd()==pwd){
        if(user.getState()=="online"){
            //该用户已经登录，不允许重复登录
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] =2;
            response["errmsg"] = "该用户已经登录，请勿重复登录";
            conn->send(response.dump());
        }
        else{
            //登陆成功，记录用户连接信息
            {
                lock_guard<mutex> lock(_connMutex);   //加锁,因为unordered_map不是线程安全的
                _userConnMap.insert({id,conn});
            }

            //登录成功,更新用户状态信息 state :offline->online
            user.setState("online");
            _userModel.updateState(user);

            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] =0;
            response["id"] =user.getId();
            response["name"] = user.getName();

            //查询该用户是否有离线消息
            vector<string> vec = _offlineMsgModel.query(id);
            if(!vec.empty()){
                response["offline"]=vec;
                //读取该用户的离线消息后，把该用户的所有离线消息删除掉
                _offlineMsgModel.remove(id);
            }

            conn->send(response.dump());
        }
    }else{
        //登录失败
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] =1;
        response["errmsg"] = "用户名或密码错误";
        conn->send(response.dump());
    }

}
//处理注册业务
void ChatService::reg(const TcpConnectionPtr &conn,json &js,Timestamp time){
    // LOG_INFO<<"do register service!!!";
    string name = js["name"];
    string pwd = js["password"];
    User user;
    user.setName(name);
    user.setPwd(pwd);
    bool state = _userModel.insert(user);
    if(state){
        //注册成功
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] =0;
        response["id"] =user.getId();
        conn->send(response.dump());
    }
    else{
        //注册失败
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(response.dump());
    };

}

void ChatService::clientCloseException(const TcpConnectionPtr& conn){
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for(auto it = _userConnMap.begin();it!=_userConnMap.end();++it){
            if(it->second == conn){
                user.setId(it->first);
                //从map表删除用户的连接信息
                _userConnMap.erase(it);
                break;
            }
        }
    }
    //更新用户的状态信息
    if(user.getId()!=-1){
        user.setState("offline");
        _userModel.updateState(user);
    }
}

 //一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn,json &js,Timestamp time){
    int toid = js["toid"].get<int>();

    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if(it != _userConnMap.end()){
            //toid在线，转发消息
            it->second->send(js.dump()); 
            // 服务器主动推送消息给toid用户
            return;
        }
    }
    //toid不在线，存储离线消息
    _offlineMsgModel.insert(toid,js.dump());
}