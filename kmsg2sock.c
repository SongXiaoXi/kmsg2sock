#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/tcp.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/kthread.h>
#include <linux/fs.h>
#include <linux/console.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/semaphore.h>
#include <linux/sched.h>

struct client_info {
	struct socket *socket;
	struct console *kmsg;
	bool need_free;
};

#define MAX_CLIENTS 10

static struct socket *listener = NULL;

static struct task_struct *server_thread = NULL;

static int server_thread_fn(void *data);

struct clients_list {
	struct client_info clients[MAX_CLIENTS];
	int count;
};

static struct clients_list clients;
static DEFINE_MUTEX(clients_mutex);

// wait kmsg2sock_server thread done
static struct semaphore sem;

static void remove_client(struct clients_list *clients, int index) {
	sock_release(clients->clients[index].socket);
	clients->clients[index].socket = NULL;
	unregister_console(clients->clients[index].kmsg);
	kfree(clients->clients[index].kmsg);
	clients->clients[index].kmsg = NULL;

	// lock mutex
	// mutex_lock(&clients_mutex);
	for (int i = index; i < clients->count - 1; i++) {
		clients->clients[i] = clients->clients[i + 1];
	}
	clients->count--;
	// mutex_unlock(&clients_mutex);
}

static void kmsg2sock_console_write(struct console *console, const char *s, unsigned int count) {

	int idx = console->index;
	if (idx < 0 || idx >= MAX_CLIENTS) {
		return;
	}

	struct client_info *client = &clients.clients[idx];
	if (client->socket == NULL || client->kmsg == NULL || client->need_free) {
		return;
	}

	struct msghdr msg = {.msg_name = NULL,
						 .msg_namelen = 0,
						 .msg_control = NULL,
						 .msg_cont9rollen = 0,
						 .msg_flags = 0};
	struct kvec iov = {.iov_base = (void *)s, .iov_len = count};

	int ret = kernel_sendmsg(client->socket, &msg, &iov, 1, count);
	if (ret < 0) {
		client->need_free = true;
	}
}

static int __init kmod_init(void) {
	sema_init(&sem, 0);
	pr_debug("kmsg2sock: init.\n");
	int ret;
	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener);
	if (ret < 0) {
		printk(KERN_ERR "kmsg2sock: Socket creation failed\n");
		return ret;
	}
	sock_set_reuseaddr(listener->sk);
	
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(2244);

	ret = listener->ops->bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr));
	if (ret < 0) {
		printk(KERN_ERR "kmsg2sock: Socket bind failed\n");
		sock_release(listener);
		listener = NULL;
		return ret;
	}

	ret = listener->ops->listen(listener, MAX_CLIENTS);
	if (ret < 0) {
		printk(KERN_ERR "kmsg2sock: Socket listen failed\n");
		sock_release(listener);
		listener = NULL;
		return ret;
	}


	server_thread = kthread_run(server_thread_fn, NULL, "kmsg2sock_server");
	if (IS_ERR(server_thread)) {
		printk(KERN_ERR "kmsg2sock: Failed to create server thread\n");
		sock_release(listener);
		listener = NULL;
		ret = PTR_ERR(server_thread);
		server_thread = NULL;
		return ret;
	}

	wake_up_process(server_thread);

	return 0;
}

static int server_thread_fn(void *data) {
	int ret;

	while (!kthread_should_stop()) {
		struct socket *client_socket = NULL;
		// scan need_free
		mutex_lock(&clients_mutex);
		for (int i = 0; i < clients.count; i++) {
			// get tcp state
			int state = clients.clients[i].socket->sk->sk_state;
			if (clients.clients[i].need_free || state == TCP_CLOSE || state == TCP_CLOSE_WAIT) {
				remove_client(&clients, i);
				clients.clients[i].need_free = false;
			}
		}
		mutex_unlock(&clients_mutex);
		
		ret = kernel_accept(listener, &client_socket, O_NONBLOCK);
		if (ret < 0) {
			if (ret != -EAGAIN) {
				printk(KERN_ERR "kmsg2sock: Socket accept failed\n");
			}
			msleep_interruptible(100);
			continue;
		}

		if (clients.count >= MAX_CLIENTS) {
			printk(KERN_ERR "kmsg2sock: Too many clients\n");
			sock_release(client_socket);
			msleep_interruptible(100);
			continue;
		}

		struct sockaddr_in client_addr;
		memset(&client_addr, 0, sizeof(client_addr));
		ret = kernel_getpeername(client_socket, (struct sockaddr *)&client_addr);
		if (ret >= 0) {
			printk(KERN_INFO "kmsg2sock: Accepted connection from %pI4:%d\n", &client_addr.sin_addr, ntohs(client_addr.sin_port));
		}

		// malloc kmsg
		struct console *kmsg = kmalloc(sizeof(struct console), GFP_KERNEL);
		if (!kmsg) {
			printk(KERN_ERR "kmsg2sock: Failed to allocate memory for kmsg\n");
			sock_release(client_socket);
			continue;
		}
		memset(kmsg, 0, sizeof(struct console));
		strncpy(kmsg->name, "kmsg2sock", sizeof(kmsg->name));
		kmsg->write = kmsg2sock_console_write;
		kmsg->flags = CON_ENABLED | CON_PRINTBUFFER;
		kmsg->index = clients.count;


		mutex_lock(&clients_mutex);
		clients.clients[clients.count].socket = client_socket;
		clients.clients[clients.count].kmsg = kmsg;
		clients.count++;
		mutex_unlock(&clients_mutex);

		register_console(kmsg);

		msleep(100);
	}

	// cleanup
	mutex_lock(&clients_mutex);
	for (int i = 0; i < clients.count; i++) {
		remove_client(&clients, i);
	}
	mutex_unlock(&clients_mutex);

	if (listener) {
		sock_release(listener);
		listener = NULL;
	}

	up(&sem);
	
	return 0;
}

static void __exit kmod_exit(void) {
	if (server_thread) {
		kthread_stop(server_thread);
		down(&sem);
		server_thread = NULL;
	} else {
		if (listener) {
			sock_release(listener);
			listener = NULL;
		}
	}

	pr_debug("kmsg2sock: exit.\n");
}

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SXX");
